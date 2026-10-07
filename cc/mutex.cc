// mutex.cc — slot claims, owner liveness, and the §7.1 env-cleanup hook.

#include "mutex.h"
#include "liveness.h"
#include "registry.h"
#include "wait.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <string.h>

namespace membridge {

void MutexCleanupIsolate(v8::Isolate* isolate);

namespace {

inline std::atomic<int32_t>* AtomicWord(int32_t* p) {
  return reinterpret_cast<std::atomic<int32_t>*>(p);
}

inline int32_t* SlotPtr(int32_t* data, uint32_t slotsWordOffset, uint32_t slot, uint32_t word) {
  return data + slotsWordOffset + slot * kMutexSlotWords + word;
}

// identity words inside a slot: pid@0, tid@1, startLo@2, startHi@3, nsLo@4, nsHi@5; gen@6, state@7
constexpr uint32_t kSlotPid = 0, kSlotTid = 1, kSlotStartLo = 2, kSlotStartHi = 3,
                   kSlotNsLo = 4, kSlotNsHi = 5, kSlotGen = 6, kSlotState = 7;

Identity SlotIdentity(int32_t* data, uint32_t slotsWordOffset, uint32_t slot) {
  int32_t* s = SlotPtr(data, slotsWordOffset, slot, 0);
  Identity id;
  id.pid = AtomicWord(s + kSlotPid)->load(std::memory_order_acquire);
  id.threadId = AtomicWord(s + kSlotTid)->load(std::memory_order_acquire);
  const uint32_t lo = static_cast<uint32_t>(AtomicWord(s + kSlotStartLo)->load());
  const uint32_t hi = static_cast<uint32_t>(AtomicWord(s + kSlotStartHi)->load());
  id.startTime = static_cast<int64_t>(lo) | (static_cast<int64_t>(hi) << 32);
  const uint32_t nlo = static_cast<uint32_t>(AtomicWord(s + kSlotNsLo)->load());
  const uint32_t nhi = static_cast<uint32_t>(AtomicWord(s + kSlotNsHi)->load());
  id.pidNsInode = static_cast<int64_t>(nlo) | (static_cast<int64_t>(nhi) << 32);
  return id;
}

uint32_t SlotGen(int32_t* data, uint32_t slotsWordOffset, uint32_t slot) {
  return static_cast<uint32_t>(
      AtomicWord(SlotPtr(data, slotsWordOffset, slot, kSlotGen))->load(std::memory_order_acquire));
}

uint32_t SlotState(int32_t* data, uint32_t slotsWordOffset, uint32_t slot) {
  return static_cast<uint32_t>(
      AtomicWord(SlotPtr(data, slotsWordOffset, slot, kSlotState))->load(std::memory_order_acquire));
}

bool CasSlotState(int32_t* data, uint32_t slotsBase, uint32_t slot, uint32_t expect, uint32_t next) {
  int32_t* p = SlotPtr(data, slotsBase, slot, kSlotState);
  int32_t exp = static_cast<int32_t>(expect);
  return AtomicWord(p)->compare_exchange_strong(exp, static_cast<int32_t>(next),
                                                std::memory_order_acq_rel);
}

void StoreSlotState(int32_t* data, uint32_t slotsBase, uint32_t slot, uint32_t v) {
  AtomicWord(SlotPtr(data, slotsBase, slot, kSlotState))
      ->store(static_cast<int32_t>(v), std::memory_order_release);
}

// ---- the slot machine (PLAN §7.2, round-3 C2/C3) ---------------------------

// Claiming word layout (round-4 S4-3/E4-6):
//   POSIX:   (nsTag8 << 24) | (pid22 << 2) | 2   — Linux pids are < 2^22
//            (PID_MAX_LIMIT), macOS pids far below; nsTag8 folds the
//            claimer's pid-namespace inode (0 = unknown / no namespaces).
//   Windows: (pid30 << 2) | 2                    — no pid namespaces.
// A pid is only meaningful inside its own namespace: containers sharing
// /dev/shm see different pids for the same process, so a claimer in another
// namespace must never be judged dead or "ours" from a bare pid.
#if defined(_WIN32)
constexpr uint32_t kClaimPidMask = 0x3FFFFFFFu;
inline uint32_t NsTag(int64_t) { return 0; }
inline uint32_t ClaimingNsTag(uint32_t) { return 0; }
#else
constexpr uint32_t kClaimPidMask = 0x3FFFFFu;
inline uint32_t NsTag(int64_t ns) {
  if (ns <= 0) return 0;
  uint64_t x = static_cast<uint64_t>(ns);
  x ^= x >> 32;
  x ^= x >> 16;
  x ^= x >> 8;
  const uint32_t t = static_cast<uint32_t>(x & 0xFFu);
  return t == 0 ? 1 : t;  // 0 is reserved for "unknown"
}
inline uint32_t ClaimingNsTag(uint32_t st) { return st >> 24; }
#endif

inline uint32_t ClaimingWord(const Identity& self) {
  return (NsTag(self.pidNsInode) << 24) |
         ((static_cast<uint32_t>(self.pid) & kClaimPidMask) << 2) | kMutexStateClaimingTag;
}
inline bool IsClaiming(uint32_t st) { return (st & 3u) == kMutexStateClaimingTag; }
inline int32_t ClaimingPid(uint32_t st) { return static_cast<int32_t>((st >> 2) & kClaimPidMask); }

enum class ClaimerVerdict { kOurs, kDead, kLiveOrUnknown };

// Who holds a Claiming word, judged ONLY within our own pid namespace. A
// different (or unknown) namespace tag is never recovered — the same
// never-steal-across-namespaces rule §7.1 applies to identities. Called under
// g_claim_mu, so "ours" means stale: no thread of this process is mid-claim.
ClaimerVerdict JudgeClaimer(uint32_t st, const Identity& self) {
  const uint32_t ourTag = NsTag(self.pidNsInode);
  if (ClaimingNsTag(st) != ourTag) return ClaimerVerdict::kLiveOrUnknown;
#if !defined(_WIN32)
  if (ourTag == 0) return ClaimerVerdict::kLiveOrUnknown;  // cannot tell namespaces apart
#endif
  const int32_t claimer = ClaimingPid(st);
  if (claimer == static_cast<int32_t>(static_cast<uint32_t>(self.pid) & kClaimPidMask)) {
    return ClaimerVerdict::kOurs;
  }
  return CheckPidAlive(claimer) == Liveness::kDead ? ClaimerVerdict::kDead
                                                   : ClaimerVerdict::kLiveOrUnknown;
}

// Same OS thread of the same process instance — the pid namespace is part of
// the identity (round-4 E4-7: two containers whose processes are both pid 1
// with the same start tick used to read as one thread).
bool SameThread(const Identity& a, const Identity& b) {
  return a.pid == b.pid && a.threadId == b.threadId && a.startTime == b.startTime &&
         a.pidNsInode == b.pidNsInode;
}

// Every claim in this process runs under this mutex: threads share a pid, so
// the Claiming word alone cannot tell two of them apart (round-3 C2/C4 — two
// workers used to publish into the same slot). Cross-process racers are
// serialized by the state-word CAS itself.
std::mutex g_claim_mu;

// Win the exclusive publish right on slot `s` from the state we observed.
bool TakePublishRight(int32_t* data, uint32_t base, uint32_t s, uint32_t seen,
                      const Identity& self) {
  return CasSlotState(data, base, s, seen, ClaimingWord(self));
}

// Publish while holding the right: bump gen (never to a value that makes the
// token 0 — token 0 IS the free word, review F12), write the identity, then
// release-store Active. Returns the new token.
uint32_t PublishHeld(int32_t* data, uint32_t base, uint32_t s, const Identity& self) {
  const uint32_t g = SlotGen(data, base, s);
  uint32_t next = g + 1;
  if (s == 0 && (next & kMutexGenMask) == 0) next++;
  AtomicWord(SlotPtr(data, base, s, kSlotGen))
      ->store(static_cast<int32_t>(next), std::memory_order_release);
  int32_t* idw = SlotPtr(data, base, s, 0);
  AtomicWord(idw + kSlotPid)->store(self.pid, std::memory_order_release);
  AtomicWord(idw + kSlotTid)->store(self.threadId, std::memory_order_release);
  AtomicWord(idw + kSlotStartLo)
      ->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) & 0xFFFFFFFFu));
  AtomicWord(idw + kSlotStartHi)
      ->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) >> 32));
  AtomicWord(idw + kSlotNsLo)
      ->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) & 0xFFFFFFFFu));
  AtomicWord(idw + kSlotNsHi)
      ->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) >> 32));
  StoreSlotState(data, base, s, kMutexStateActive);
  return (s << 16) | (next & kMutexGenMask);
}

// Take a Free slot, or recover one whose claimer died mid-publish. Called
// under g_claim_mu, so a Claiming word carrying OUR pid is stale (no thread of
// this process is mid-claim) — e.g. a crashed process whose pid we recycled.
bool TakeFreeOrAbandoned(int32_t* data, uint32_t base, uint32_t s, const Identity& self) {
  const uint32_t st = SlotState(data, base, s);
  if (st == kMutexStateFree) return TakePublishRight(data, base, s, st, self);
  if (IsClaiming(st) && JudgeClaimer(st, self) != ClaimerVerdict::kLiveOrUnknown) {
    return TakePublishRight(data, base, s, st, self);
  }
  return false;
}

// Reclaim an Active slot whose identity is provably dead. The right is won
// from (Active, gen g) where g was read BEFORE the liveness verdict: if any
// other claimer published in between, gen moved and we back off (round-3 C3 —
// the publish used to re-read gen fresh, so a second reclaimer overwrote a
// live claimer's identity).
bool TakeDeadActive(int32_t* data, uint32_t base, uint32_t s, const Identity& self,
                    const int32_t* lockWord) {
  if (SlotState(data, base, s) != kMutexStateActive) return false;
  const uint32_t g = SlotGen(data, base, s);
  const uint32_t staleToken = (s << 16) | (g & kMutexGenMask);
  auto lockedBy = [&]() {
    if (lockWord == nullptr) return false;
    const uint32_t cur = static_cast<uint32_t>(
        AtomicWord(const_cast<int32_t*>(lockWord))->load(std::memory_order_acquire));
    return (cur & kMutexTokenMask) == staleToken;
  };
  // §7.2: a dead HOLDER's slot is freed by the steal path, never reclaimed here.
  if (lockedBy()) return false;
  if (CheckLiveness(SlotIdentity(data, base, s)) != Liveness::kDead) return false;
  if (!TakePublishRight(data, base, s, kMutexStateActive, self)) return false;
  if (SlotGen(data, base, s) != g || lockedBy()) {
    StoreSlotState(data, base, s, kMutexStateActive);  // someone else's slot now
    return false;
  }
  return true;
}

// ---- claim references for the env-cleanup hook ------------------------------

// Segment identity for slot bookkeeping: two mappings of one segment have
// different base addresses, so entries are matched by object identity.
struct SegKey {
  int32_t* data = nullptr;
  int64_t dev = 0;
  int64_t ino = 0;
  std::string name;
};

bool SameSegment(const SegKey& a, const SegKey& b) {
  if (a.ino != 0 || b.ino != 0) return a.dev == b.dev && a.ino == b.ino;
  if (!a.name.empty() && !b.name.empty()) return a.name == b.name;  // Windows
  return a.data == b.data;
}

SegKey KeyFor(int32_t* data, const std::string& name) {
  SegKey k;
  k.data = data;
  k.name = name;
  // Round-4 P4-1: the name lookup is O(log n); scanning every live mapping
  // (FindByAddress) made each claim O(mappings) — 241–333 µs at 10k. The
  // address scan stays as the fallback for a name that now refers to a
  // different mapping (grown, or unlinked + recreated).
  auto covers = [data](const std::shared_ptr<Mapping>& m) {
    const char* base = static_cast<const char*>(m->base);
    const char* p = reinterpret_cast<const char*>(data);
    return m->base != nullptr && p >= base && p < base + m->mappingBytes;
  };
  std::shared_ptr<Mapping> m = Registry::Get().Find(name);
  if (m == nullptr || !covers(m)) m = Registry::FindByAddress(data);
  if (m != nullptr) {
    k.dev = m->dev;
    k.ino = m->ino;
  }
  return k;
}

struct HeldEntry {
  SegKey seg;
  uint32_t token = 0;
  int slot = -1;
  int refs = 0;
  // role entries (§8): the token is cleared from `roleWordOffset` on release
  bool isRole = false;
  uint32_t roleWordOffset = 0;
  // The SAB's own BackingStore: keeps the pages mapped while the entry lives
  // (independent of the registry, so it survives unlink — R11/F14).
  std::shared_ptr<v8::BackingStore> pin;
};

struct HeldState {
  std::vector<HeldEntry> entries;
  bool hookRegistered = false;
};

std::mutex g_held_mu;
std::map<v8::Isolate*, HeldState> g_held;

// Caller holds g_held_mu.
void AddRefLocked(v8::Isolate* isolate, HeldEntry e) {
  HeldState& st = g_held[isolate];
  if (!st.hookRegistered) {
    st.hookRegistered = true;
    node::AddEnvironmentCleanupHook(
        isolate,
        [](void* p) { MutexCleanupIsolate(static_cast<v8::Isolate*>(p)); },
        isolate);
  }
  for (HeldEntry& h : st.entries) {
    if (h.seg.data == e.seg.data && h.slot == e.slot && h.isRole == e.isRole) {
      h.refs++;
      h.token = e.token;  // a re-claim after a gen bump refreshes the token
      if (h.pin == nullptr) h.pin = std::move(e.pin);
      return;
    }
  }
  e.refs = 1;
  st.entries.push_back(std::move(e));
}

// Release the lock `h.token` holds (tolerating HAS_WAITERS) as OWNER_DIED and
// wake one waiter. Returns false when the word no longer holds the token.
// The flag is published BEFORE the word is freed (round-4 E4-3): the next
// acquirer's exchange of OWNER_DIED happens after it observes the word free,
// so it can never miss the death and leave the flag for a later holder.
bool ReleaseLockWord(const HeldEntry& h) {
  auto* lockWord = AtomicWord(h.seg.data);
  auto* ownerDied = AtomicWord(h.seg.data + 1);
  int32_t cur = lockWord->load(std::memory_order_acquire);
  bool flagged = false;
  for (;;) {
    if (static_cast<uint32_t>(cur) != h.token &&
        static_cast<uint32_t>(cur) != (h.token | kMutexHasWaiters)) {
      // No longer ours (stolen meanwhile): withdraw a flag we set — the
      // stealer reports the death itself.
      if (flagged) ownerDied->store(0, std::memory_order_release);
      return false;
    }
    if (!flagged) {
      ownerDied->store(1, std::memory_order_release);
      flagged = true;
    }
    if (lockWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) break;
  }
  SyncWake(h.seg.data, 1);
  return true;
}

void FreeOwnSlot(int32_t* data, uint32_t base, uint32_t slot, const Identity& self) {
  if (SlotState(data, base, slot) != kMutexStateActive) return;
  if (!SameThread(SlotIdentity(data, base, slot), self)) return;
  CasSlotState(data, base, slot, kMutexStateActive, kMutexStateFree);
}

void ReleaseRole(const HeldEntry& h) {
  auto* roleWord = AtomicWord(h.seg.data + h.roleWordOffset);
  int32_t cur = roleWord->load(std::memory_order_acquire);
  if (static_cast<uint32_t>(cur) == h.token) {
    roleWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel);
  }
  FreeOwnSlot(h.seg.data, kRingSlotsWordOffset, static_cast<uint32_t>(h.slot), SelfIdentity());
}

}  // namespace

// ---- mutex claims ------------------------------------------------------------

uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name,
                        v8::Local<v8::SharedArrayBuffer> sab) {
  const uint32_t base = kMutexHeaderWords;
  const Identity self = SelfIdentity();
  if (self.startTime < 0) {
    // Refuse to claim with an unrecordable identity (review F25): a slot
    // stamped startTime=-1 would let a later check judge its LIVE owner dead.
    ThrowError(isolate, "E_SYSTEM",
               "cannot establish thread identity (/proc unavailable); refusing to claim a mutex slot",
               name);
  }
  HeldEntry e;
  e.seg = KeyFor(data, name);
  if (!sab.IsEmpty()) e.pin = sab->GetBackingStore();

  std::lock_guard<std::mutex> claim(g_claim_mu);
  uint32_t token = 0;
  int slot = -1;
  // Pass 1 (review P8): this thread's live slot — no liveness checks.
  for (uint32_t s = 0; s < kMutexSlotCount && slot < 0; s++) {
    if (SlotState(data, base, s) == kMutexStateActive &&
        SameThread(SlotIdentity(data, base, s), self)) {
      token = (s << 16) | (SlotGen(data, base, s) & kMutexGenMask);
      if (token == 0) {
        // Token 0 IS the free word (review F12): lock() would "acquire" with a
        // CAS 0 -> 0 and never exclude anyone. A published slot never carries
        // it (PublishHeld skips it), so only corrupted slot memory gets here —
        // re-publish our own slot under the right so the gen moves off it.
        if (!TakePublishRight(data, base, s, kMutexStateActive, self)) continue;
        token = PublishHeld(data, base, s, self);
      }
      slot = static_cast<int>(s);
    }
  }
  // Pass 2: a Free slot, or one abandoned mid-publish by a dead claimer.
  for (uint32_t s = 0; s < kMutexSlotCount && slot < 0; s++) {
    if (TakeFreeOrAbandoned(data, base, s, self)) {
      token = PublishHeld(data, base, s, self);
      slot = static_cast<int>(s);
    }
  }
  // Pass 3 (§7.2): reclaim a provably dead participant's slot.
  for (uint32_t s = 0; s < kMutexSlotCount && slot < 0; s++) {
    if (TakeDeadActive(data, base, s, self, data)) {
      token = PublishHeld(data, base, s, self);
      slot = static_cast<int>(s);
    }
  }
  if (slot < 0) {
    ThrowError(isolate, "E_TIMEOUT",
               "no mutex participant slot available for thread (all " +
                   std::to_string(kMutexSlotCount) + " slots held by live threads)",
               name);
  }
  e.token = token;
  e.slot = slot;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);  // order: g_claim_mu -> g_held_mu
    AddRefLocked(isolate, std::move(e));
  }
  return token;
}

bool MutexOwnerAlive(int32_t* data, uint32_t token, uint32_t slotsWordOffset,
                     uint32_t slotCount) {
  const uint32_t slot = (token & kMutexTokenMask) >> 16;
  const uint32_t gen = token & kMutexGenMask;
  if (slot >= slotCount) return true;  // malformed: never steal
  const uint32_t st = SlotState(data, slotsWordOffset, slot);
  if (IsClaiming(st)) return true;  // identity mid-write: never judge it; retry later
  // A gen mismatch means the slot was claimed again after this token was
  // issued (reclaim only takes dead identities; a Free slot is re-published
  // with a gen bump): no live thread can present this token again (F13).
  const uint32_t g0 = SlotGen(data, slotsWordOffset, slot);
  if ((g0 & kMutexGenMask) != gen) return false;
  // Freed without releasing the word (round-3: a Free slot's token can never
  // be presented again — the old "non-Active = alive" rule wedged the word).
  if (st == kMutexStateFree) return false;
  if (CheckLiveness(SlotIdentity(data, slotsWordOffset, slot)) != Liveness::kDead) {
    return true;  // unknown liveness -> alive (never steal, §7.1)
  }
  // The verdict must be about the slot we looked at: a claim that started
  // while we read the identity invalidates it (report alive = retry later).
  return SlotState(data, slotsWordOffset, slot) != kMutexStateActive ||
         SlotGen(data, slotsWordOffset, slot) != g0;
}

void MutexReleaseThreadSlots(int32_t* data) {
  const Identity self = SelfIdentity();
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    FreeOwnSlot(data, kMutexHeaderWords, s, self);
  }
}

void MutexCleanupIsolate(v8::Isolate* isolate) {
  std::vector<HeldEntry> held;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    held.swap(it->second.entries);
    g_held.erase(it);
  }
  for (const HeldEntry& h : held) {
    // An entry with no pin has unmapped memory behind it — never touch it (R11).
    if (h.pin == nullptr) continue;
    if (h.isRole) {
      ReleaseRole(h);  // §8: a dead thread's role is cleared and its slot freed
    } else {
      ReleaseLockWord(h);  // §7.1: OWNER_DIED + wake if this thread still held it
    }
  }
  // Review F2: free this thread's participant slots in every claimed segment.
  for (const HeldEntry& h : held) {
    if (!h.isRole && h.pin != nullptr) MutexReleaseThreadSlots(h.seg.data);
  }
}

void MutexUnregisterClaim(v8::Isolate* isolate, int32_t* data, int slot, bool releaseHeld) {
  HeldEntry dropped;
  bool found = false, last = false, slotStillUsed = false;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    auto& entries = it->second.entries;
    for (auto vit = entries.begin(); vit != entries.end(); ++vit) {
      if (vit->seg.data == data && vit->slot == slot && !vit->isRole) {
        found = true;
        dropped = *vit;  // copy keeps the pin alive through the release below
        if (--vit->refs <= 0) {
          last = true;
          entries.erase(vit);
        }
        break;
      }
    }
    if (found && last) {
      // Another mapping of the SAME segment on this thread shares the slot
      // (slots are per thread, not per mapping): keep it while that lives.
      for (const HeldEntry& h : entries) {
        if (!h.isRole && h.slot == slot && SameSegment(h.seg, dropped.seg)) {
          slotStillUsed = true;
          break;
        }
      }
    }
  }
  if (!found || dropped.pin == nullptr) return;
  // Only the dropped instance's OWN hold is released (round-3 C1): a sibling
  // instance on this thread shares the token, but not the hold.
  // When no instance remains that could present the token, a word still
  // holding it is orphaned — release it too rather than wedge the mutex.
  const bool orphaned = last && !slotStillUsed;
  if (releaseHeld || orphaned) ReleaseLockWord(dropped);
  if (orphaned) {
    FreeOwnSlot(data, kMutexHeaderWords, static_cast<uint32_t>(slot), SelfIdentity());
  }
}

// ---- §8 ring role claims ------------------------------------------------------

namespace {

uint32_t ClaimRoleLocked(v8::Isolate* isolate, const std::string& name, int32_t* data,
                         uint32_t base, uint32_t roleWordOffset, uint32_t slotIndex,
                         bool isProducer, const Identity& self, bool* outReclaim,
                         std::unique_lock<std::mutex>& claimLock) {
  auto* roleWord = AtomicWord(data + roleWordOffset);
  *outReclaim = false;
  // A live participant caught mid-claim (Claiming word, or published slot
  // with the role word still 0) settles within microseconds — it either wins
  // the role word or backs off. Give it a bounded window before answering
  // E_ROLE_TAKEN, instead of failing a claim the other side may yet lose
  // (round-3 post-verification nit 2). g_claim_mu is RELEASED across each
  // sleep (round-4 P4-4: holding it stalled every other claim in the process
  // for the whole window); the loop re-reads all state after re-locking.
  using Clock = std::chrono::steady_clock;
  constexpr auto kSettleWindow = std::chrono::milliseconds(100);
  // Whole-claim wall-clock cap (round-4 S4-11): a writer racing the slot
  // words cannot keep this loop busy for more than ~1 s.
  const Clock::time_point claimDeadline = Clock::now() + std::chrono::seconds(1);
  Clock::time_point settleStart{};
  bool settling = false;
  int settleDelayUs = 20;
  auto stillSettling = [&]() {
    if (!settling) {
      settling = true;
      settleStart = Clock::now();
    }
    if (Clock::now() - settleStart >= kSettleWindow) return false;
    claimLock.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(settleDelayUs));
    claimLock.lock();
    if (settleDelayUs < 1000) settleDelayUs *= 2;
    return true;
  };
  // Bounded (review F23): a wedged slot fails with E_TIMEOUT, never spins.
  for (int guard = 0; guard < 100000 && Clock::now() < claimDeadline; guard++) {
    uint32_t cur = static_cast<uint32_t>(roleWord->load(std::memory_order_acquire));
    if (cur != 0) {
      const uint32_t curSlot = (cur & kMutexTokenMask) >> 16;
      if (curSlot == slotIndex &&
          SlotState(data, base, slotIndex) == kMutexStateActive &&
          SameThread(SlotIdentity(data, base, slotIndex), self)) {
        // This thread already holds the role. The JS layer hands out its live
        // producer instance instead of calling here; reaching this means the
        // previous instance is unreachable but not yet finalized — share the
        // role (refcounted). A consumer is exclusive per instance: two live
        // consumers could both peek and double-process a message (R15b).
        if (isProducer) {
          *outReclaim = true;
          return cur;
        }
        ThrowError(isolate, "E_ROLE_TAKEN",
                   "ring consumer already open on this thread (close() the previous instance)",
                   name);
      }
      if (MutexOwnerAlive(data, cur, base, 2)) {
        ThrowError(isolate, "E_ROLE_TAKEN", "ring role already held by a live participant", name);
      }
      int32_t expected = static_cast<int32_t>(cur);
      if (!roleWord->compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) continue;
    }

    // The role word is free. Win the role's slot.
    const uint32_t st = SlotState(data, base, slotIndex);
    bool haveRight = false;
    if (st == kMutexStateActive) {
      const Identity id = SlotIdentity(data, base, slotIndex);
      if (SameThread(id, self)) {
        // Our own slot behind a cleared role word. Releases run under
        // g_claim_mu, so this is defensive only; re-publish under the right
        // so the gen moves and any token minted for the old claim goes stale.
        haveRight = TakePublishRight(data, base, slotIndex, kMutexStateActive, self);
      } else if (CheckLiveness(id) != Liveness::kDead) {
        // A live participant is between its slot publish and its role CAS
        // (or its release cleared the word before freeing the slot).
        if (stillSettling()) continue;
        ThrowError(isolate, "E_ROLE_TAKEN", "ring role is being claimed by a live participant",
                   name);
      } else {
        haveRight = TakeDeadActive(data, base, slotIndex, self, nullptr);
      }
    } else if (IsClaiming(st) && JudgeClaimer(st, self) == ClaimerVerdict::kLiveOrUnknown) {
      if (stillSettling()) continue;
      ThrowError(isolate, "E_ROLE_TAKEN", "ring role is being claimed by a live participant",
                 name);
    } else {
      haveRight = TakeFreeOrAbandoned(data, base, slotIndex, self);
    }
    if (!haveRight) continue;
    const uint32_t token = PublishHeld(data, base, slotIndex, self);
    int32_t zero = 0;
    if (roleWord->compare_exchange_strong(zero, static_cast<int32_t>(token),
                                          std::memory_order_acq_rel)) {
      return token;
    }
    // Another process installed a token between our clear and our publish —
    // the slot we just published is OURS (we hold it), so freeing it can never
    // free someone else's (round-3 C4: the loser used to free the winner's).
    CasSlotState(data, base, slotIndex, kMutexStateActive, kMutexStateFree);
  }
  ThrowError(isolate, "E_TIMEOUT", "ring role claim made no progress (slot stuck?)", name);
}

}  // namespace

uint32_t RingClaimRole(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t roleWord, uint32_t slotIndex, bool isProducer,
                       v8::Local<v8::SharedArrayBuffer> sab) {
  const Identity self = SelfIdentity();
  if (self.startTime < 0) {
    // Refuse to claim with an unrecordable identity (review R19): a role slot
    // stamped startTime=-1 is judged Unknown = never steal.
    ThrowError(isolate, "E_SYSTEM",
               "cannot establish thread identity (/proc unavailable); refusing to claim a ring role",
               name);
  }
  HeldEntry e;
  e.seg = KeyFor(data, name);
  e.isRole = true;
  e.roleWordOffset = roleWord;
  e.slot = static_cast<int>(slotIndex);
  if (!sab.IsEmpty()) e.pin = sab->GetBackingStore();

  std::unique_lock<std::mutex> claim(g_claim_mu);
  bool shared = false;
  const uint32_t token = ClaimRoleLocked(isolate, name, data, kRingSlotsWordOffset, roleWord,
                                         slotIndex, isProducer, self, &shared, claim);
  e.token = token;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    AddRefLocked(isolate, std::move(e));
  }
  return token;
}

void MutexUnregisterRole(v8::Isolate* isolate, int32_t* data, int slot) {
  HeldEntry dropped;
  bool last = false, roleStillHeld = false;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    auto& entries = it->second.entries;
    for (auto vit = entries.begin(); vit != entries.end(); ++vit) {
      if (vit->seg.data == data && vit->slot == slot && vit->isRole) {
        dropped = *vit;
        if (--vit->refs <= 0) {
          last = true;
          entries.erase(vit);
        }
        break;
      }
    }
    if (last) {
      // Another mapping of the SAME ring on this thread still holds the role
      // (round-4 E4-10, parity with MutexUnregisterClaim): releasing it now
      // would let another process take a role a live instance still uses.
      for (const HeldEntry& h : entries) {
        if (h.isRole && h.slot == slot && SameSegment(h.seg, dropped.seg)) {
          roleStillHeld = true;
          break;
        }
      }
    }
  }
  if (!last || roleStillHeld || dropped.pin == nullptr) return;  // R11: never touch unmapped memory
  std::lock_guard<std::mutex> claim(g_claim_mu);  // a release must not interleave a claim
  ReleaseRole(dropped);
}

}  // namespace membridge
