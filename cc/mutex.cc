// mutex.cc — slot claims, owner liveness, and the §7.1 env-cleanup hook.

#include "mutex.h"
#include "liveness.h"
#include "registry.h"
#include "wait.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include <string.h>

namespace membridge {

void MutexCleanupIsolate(v8::Isolate* isolate);
void MutexRegisterClaim(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot);
void MutexTrackRole(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot,
                    uint32_t roleWordOffset);

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

}  // namespace

void MutexTrackRole(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot,
                    uint32_t roleWordOffset);

// Publish `self` into slot `s` and return the token. The gen CAS here is the
// EXCLUSIVE publish right (review F23: with the old RESERVED state gone, this
// is the only serialization between reclaim racers) — it is attempted ONCE:
// on failure the caller rescans, never retries blindly, because a retry would
// overwrite the identity a winning racer just published (two owners on one
// slot). Gen bumps skip values that would make the token 0 (slot 0, gen 0):
// token 0 IS the free word — a claim producing it would break mutual
// exclusion (review F12).
namespace {
bool PublishClaimedSlot(int32_t* data, uint32_t slotsBase, uint32_t s,
                        const Identity& self, uint32_t* outToken) {
  uint32_t g = SlotGen(data, slotsBase, s);
  int32_t gExpected = static_cast<int32_t>(g);
  uint32_t next = g + 1;
  if (s == 0 && (next & kMutexGenMask) == 0) next++;  // never token 0
  if (!AtomicWord(SlotPtr(data, slotsBase, s, kSlotGen))
           ->compare_exchange_strong(gExpected, static_cast<int32_t>(next),
                                     std::memory_order_acq_rel)) {
    return false;
  }
  int32_t* idw = SlotPtr(data, slotsBase, s, 0);
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
  AtomicWord(SlotPtr(data, slotsBase, s, kSlotState))
      ->store(static_cast<int32_t>(kMutexStateActive), std::memory_order_release);
  *outToken = (s << 16) | (next & kMutexGenMask);
  return true;
}
}  // namespace

uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name) {
  const uint32_t slotsBase = kMutexHeaderWords;
  const Identity self = SelfIdentity();
  if (self.startTime < 0) {
    // Refuse to claim with an unrecordable identity (review F25): a slot
    // stamped startTime=-1 would let a later check judge its LIVE owner dead.
    ThrowError(isolate, "E_SYSTEM",
               "cannot establish thread identity (/proc unavailable); refusing to claim a mutex slot",
               name);
  }

  // Pass 1 (review P8): reuse this thread's slot or take a FREE slot — no
  // liveness checks on the way.
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) == kMutexStateActive &&
        AtomicWord(SlotPtr(data, slotsBase, s, kSlotPid))->load() == self.pid &&
        AtomicWord(SlotPtr(data, slotsBase, s, kSlotTid))->load() == self.threadId &&
        SlotIdentity(data, slotsBase, s).startTime == self.startTime) {
      uint32_t token = (s << 16) | (SlotGen(data, slotsBase, s) & kMutexGenMask);
      if (token == 0) {
        if (!PublishClaimedSlot(data, slotsBase, s, self, &token)) continue;  // bumps off 0
      }
      MutexRegisterClaim(isolate, data, token, static_cast<int>(s),
                         v8::Local<v8::SharedArrayBuffer>());
      return token;
    }
  }
  // Free/reserving slots (review F23): the slot's PID word doubles as the
  // publish-claim marker. A virgin slot has pid 0 — win the publish right by
  // CASing our pid in (state stays Free until the identity is fully published
  // and the slot flips to Active). A slot with a pid is mid-publish by that
  // thread: recoverable exactly when that pid is provably dead (no startTime
  // comparison — the start words may still belong to the previous owner).
  // Free slots normally have pid 0 (release zeroes it), so the common path
  // still performs no liveness syscalls (review P8).
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) != kMutexStateFree) continue;
    auto* pidWord = AtomicWord(SlotPtr(data, slotsBase, s, kSlotPid));
    const int32_t pidNow = pidWord->load(std::memory_order_acquire);
    int32_t expectedPid = pidNow;
    if (pidNow == 0) {
      if (!pidWord->compare_exchange_strong(expectedPid, static_cast<int32_t>(self.pid),
                                            std::memory_order_acq_rel)) {
        continue;  // raced another claimer: retry
      }
    } else if (pidNow == static_cast<int32_t>(self.pid)) {
      // our own crashed mid-publish slot: reuse the right we already hold
    } else if (CheckPidAlive(pidNow) == Liveness::kDead) {
      if (!pidWord->compare_exchange_strong(expectedPid, static_cast<int32_t>(self.pid),
                                            std::memory_order_acq_rel)) {
        continue;
      }
    } else {
      continue;  // a live thread holds the publish right
    }
    uint32_t token = 0;
    if (!PublishClaimedSlot(data, slotsBase, s, self, &token)) continue;
    MutexRegisterClaim(isolate, data, token, static_cast<int>(s),
                       v8::Local<v8::SharedArrayBuffer>());
    return token;
  }

  // Pass 2 (review F13): reclaim a slot whose identity is provably dead and
  // whose token is not the current lockWord value (§7.2). The CAS target is
  // the GEN word, not the state word: only the reclaimer bumps a dead slot's
  // gen, so winning it proves no concurrent claim intervened (the state-CAS
  // version was ABA-prone: R scans a dead slot, Q reclaims and publishes it,
  // R's stale state-CAS still succeeds and overwrites Q's identity). The
  // identity rewrite happens in place under the gen-CAS — the state stays
  // Active throughout (review F23: there is no RESERVED state anymore).
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) != kMutexStateActive) continue;
    uint32_t staleToken = (s << 16) | (SlotGen(data, slotsBase, s) & kMutexGenMask);
    const uint32_t currentLock = static_cast<uint32_t>(
        AtomicWord(data)->load(std::memory_order_acquire)) & kMutexTokenMask;
    if (staleToken == currentLock) continue;
    if (CheckLiveness(SlotIdentity(data, slotsBase, s)) != Liveness::kDead) continue;
    uint32_t token = 0;
    if (!PublishClaimedSlot(data, slotsBase, s, self, &token)) continue;
    MutexRegisterClaim(isolate, data, token, static_cast<int>(s), v8::Local<v8::SharedArrayBuffer>());
    return token;
  }

  ThrowError(isolate, "E_TIMEOUT",
             "no mutex participant slot available for thread (all " +
                 std::to_string(kMutexSlotCount) + " slots held by live threads)",
             name);
}

bool MutexOwnerAlive(int32_t* data, uint32_t token, uint32_t slotsWordOffset,
                     uint32_t slotCount) {
  const uint32_t slot = (token & kMutexTokenMask) >> 16;
  const uint32_t gen = token & kMutexGenMask;
  if (slot >= slotCount) return true;   // malformed: never steal
  // Review F13: a gen mismatch means the slot was RECLAIMED after this token
  // was issued — its holder died (reclaim only takes dead identities) and no
  // live thread can ever present this token again. Treat it as dead so the
  // word does not wedge forever; the steal CAS still has to win.
  if ((SlotGen(data, slotsWordOffset, slot) & kMutexGenMask) != gen) return false;
  const uint32_t state = SlotState(data, slotsWordOffset, slot);
  if (state != kMutexStateActive) return true;
  const Liveness l = CheckLiveness(SlotIdentity(data, slotsWordOffset, slot));
  return l != Liveness::kDead;  // unknown liveness -> alive (never steal, §7.1)
}

// ---- held-lock registry for the env-cleanup hook ---------------------------

namespace {

struct HeldLock {
  int32_t* data;
  uint32_t token;
  int slot;
  // role entries (§8): the token is cleared from `roleWordOffset` on teardown
  bool isRole = false;
  uint32_t roleWordOffset = 0;
  // R11/F14: the pin is the SAB's own BackingStore (survives unlink); the
  // registry Mapping pin is kept for role entries claimed before this field.
  std::shared_ptr<v8::BackingStore> bsPin;
  std::shared_ptr<Mapping> pin;
};

struct HeldState {
  std::vector<HeldLock> locks;
  std::vector<int32_t*> claimedData;  // every mutex segment this isolate claimed on
  bool hookRegistered = false;
};

std::mutex g_held_mu;
std::map<v8::Isolate*, HeldState> g_held;

}  // namespace

// Claim-time registration (review P1/F2/F14/R11/R12): one native call per
// Mutex instance (its lazy slot claim), zero per lock()/unlock(). The pin is
// the SAB's own BackingStore — independent of the registry, so it survives
// unlink and releases with the entry. The teardown hook releases anything
// still held and frees this thread's slots; MutexUnregisterClaim (called
// from a FinalizationRegistry when the JS Mutex is collected) drops the
// entry and its pin earlier, so unlinked segments do not pin pages and fds
// for the isolate's lifetime.
void MutexRegisterClaim(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot,
                        v8::Local<v8::SharedArrayBuffer> sab) {
  std::shared_ptr<v8::BackingStore> bs;
  if (!sab.IsEmpty()) bs = sab->GetBackingStore();
  std::lock_guard<std::mutex> lock(g_held_mu);
  HeldState& st = g_held[isolate];
  if (!st.hookRegistered) {
    st.hookRegistered = true;
    node::AddEnvironmentCleanupHook(
        isolate,
        [](void* p) { MutexCleanupIsolate(static_cast<v8::Isolate*>(p)); },
        isolate);
  }
  bool knownData = false;
  for (int32_t* d : st.claimedData) {
    if (d == data) {
      knownData = true;
      break;
    }
  }
  if (!knownData) st.claimedData.push_back(data);
  for (HeldLock& h : st.locks) {
    if (h.data == data && h.slot == slot && !h.isRole) {
      h.token = token;  // re-claim after a gen bump: refresh the token
      h.bsPin = bs;
      return;
    }
  }
  st.locks.push_back({data, token, slot, false, 0, std::move(bs), nullptr});
}


void MutexReleaseThreadSlots(int32_t* data) {
  const uint32_t slotsBase = kMutexHeaderWords;
  const Identity self = SelfIdentity();
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) == kMutexStateActive &&
        SlotIdentity(data, slotsBase, s).pid == self.pid &&
        SlotIdentity(data, slotsBase, s).threadId == self.threadId) {
      // Zero the pid BEFORE freeing (review F23): a Free slot must read
      // pid 0 so claimers can tell virgin slots from crashed mid-publish ones.
      AtomicWord(SlotPtr(data, slotsBase, s, kSlotPid))->store(0, std::memory_order_release);
      CasSlotState(data, slotsBase, s, kMutexStateActive, kMutexStateFree);
    }
  }
}

namespace {

void ReleaseHeldLock(const HeldLock& h) {
  // CAS the token word to 0 (tolerating the HAS_WAITERS bit), mark ownerDied,
  // and wake one waiter. Mirrors §7.1: the hook marks every lock the dead
  // thread holds as OWNER_DIED and wakes waiters.
  auto* lockWord = AtomicWord(h.data);
  int32_t cur = lockWord->load(std::memory_order_acquire);
  for (;;) {
    if (static_cast<uint32_t>(cur) != h.token &&
        static_cast<uint32_t>(cur) != (h.token | kMutexHasWaiters)) {
      return;  // no longer ours
    }
    if (lockWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) break;
  }
  AtomicWord(h.data + 1)->store(1, std::memory_order_release);  // ownerDied
  SyncWake(h.data, 1);
  // Zero the pid BEFORE freeing (review F23 invariant: Free ⇒ pid 0).
  AtomicWord(SlotPtr(h.data, kMutexHeaderWords, static_cast<uint32_t>(h.slot), kSlotPid))
      ->store(0, std::memory_order_release);
  CasSlotState(h.data, kMutexHeaderWords, static_cast<uint32_t>(h.slot), kMutexStateActive,
               kMutexStateFree);
}

}  // namespace

void MutexCleanupIsolate(v8::Isolate* isolate) {
  std::vector<HeldLock> held;
  std::vector<int32_t*> datas;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    held.swap(it->second.locks);
    datas.swap(it->second.claimedData);
    g_held.erase(it);
  }
  for (const HeldLock& h : held) {
    // R11: an entry with NO pin has unmapped memory behind it — never touch
    // its words (a claim-after-unlink used to SIGSEGV the teardown hook).
    if (h.bsPin == nullptr && h.pin == nullptr) continue;
    if (h.isRole) {
      // §8 role claim of a dead thread: clear the role word (if still ours)
      // and free the slot so a new participant can take over.
      auto* roleWord = AtomicWord(h.data + h.roleWordOffset);
      int32_t cur = roleWord->load(std::memory_order_acquire);
      if (static_cast<uint32_t>(cur) == h.token) {
        roleWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel);
      }
      // Zero the pid BEFORE freeing (review F23 invariant: Free ⇒ pid 0).
      AtomicWord(SlotPtr(h.data, kRingSlotsWordOffset, static_cast<uint32_t>(h.slot), kSlotPid))
          ->store(0, std::memory_order_release);
      CasSlotState(h.data, kRingSlotsWordOffset, static_cast<uint32_t>(h.slot),
                   kMutexStateActive, kMutexStateFree);
    } else {
      ReleaseHeldLock(h);
    }
  }
  // Review F2: free this thread's participant slots in every claimed segment
  // whose pages are still pinned (unpinned ones are unmapped — skip, R11).
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    // dropped in MutexUnregisterClaim; nothing extra needed here
  }
  for (const HeldLock& h : held) {
    if (!h.isRole && (h.bsPin != nullptr || h.pin != nullptr)) {
      MutexReleaseThreadSlots(h.data);
    }
  }
}

// JS Mutex collected (FinalizationRegistry / close()): drop the entry and
// its pin. If the collected instance still held the lock, release it as
// OWNER_DIED (the JS object is gone; nobody will unlock).
void MutexUnregisterClaim(v8::Isolate* isolate, int32_t* data, int slot) {
  std::vector<HeldLock> dropped;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    auto& locks = it->second.locks;
    for (auto vit = locks.begin(); vit != locks.end();) {
      if (vit->data == data && vit->slot == slot && !vit->isRole) {
        dropped.push_back(*vit);
        vit = locks.erase(vit);
      } else {
        ++vit;
      }
    }
    for (auto dit = it->second.claimedData.begin(); dit != it->second.claimedData.end();) {
      if (*dit == data) {
        // keep the segment claimed if other slots on it remain
        bool stillUsed = false;
        for (const HeldLock& h : locks) {
          if (h.data == *dit) {
            stillUsed = true;
            break;
          }
        }
        if (!stillUsed) {
          dit = it->second.claimedData.erase(dit);
        } else {
          ++dit;
        }
      } else {
        ++dit;
      }
    }
  }
  for (const HeldLock& h : dropped) {
    ReleaseHeldLock(h);
  }
}

// JS RingConsumer collected (FinalizationRegistry / close()) — review R15b:
// release the role so a replacement consumer can open on this thread without
// waiting for the thread to exit. The role word is cleared only if it still
// holds our token; the slot is freed with the Free⇒pid-0 invariant. Only the
// consumer registers for GC release (a collected producer's role must stay
// held: a replacement producer joining while a live sibling still writes
// would break SPSC — the producer role releases at thread teardown).
void MutexUnregisterRole(v8::Isolate* isolate, int32_t* data, int slot) {
  HeldLock dropped;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    auto& locks = it->second.locks;
    for (auto vit = locks.begin(); vit != locks.end(); ++vit) {
      if (vit->data == data && vit->slot == slot && vit->isRole) {
        dropped = *vit;
        found = true;
        locks.erase(vit);
        break;
      }
    }
  }
  if (!found) return;
  if (dropped.bsPin == nullptr && dropped.pin == nullptr) return;  // R11: unmapped
  auto* roleWord = AtomicWord(dropped.data + dropped.roleWordOffset);
  int32_t cur = roleWord->load(std::memory_order_acquire);
  if (static_cast<uint32_t>(cur) == dropped.token) {
    roleWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel);
  }
  AtomicWord(SlotPtr(dropped.data, kRingSlotsWordOffset, static_cast<uint32_t>(dropped.slot),
                     kSlotPid))
      ->store(0, std::memory_order_release);
  CasSlotState(dropped.data, kRingSlotsWordOffset, static_cast<uint32_t>(dropped.slot),
               kMutexStateActive, kMutexStateFree);
}

// ---- §8 ring role claims ----------------------------------------------------

namespace {

// A role claim is a restricted slot claim: the role owns exactly one slot.
// `isProducer` gates the same-thread re-claim (review R15b): two live producer
// handles on one thread are harmless (the data path is per-thread), but two
// live CONSUMER instances could both peek the same message and double-process
// it, so a same-thread consumer re-claim is E_ROLE_TAKEN.
uint32_t ClaimRoleSlot(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t slotsBase, uint32_t roleWordOffset, uint32_t slotIndex,
                       bool isProducer) {
  const Identity self = SelfIdentity();
  if (self.startTime < 0) {
    // Refuse to claim with an unrecordable identity (review R19, matching the
    // mutex claim): a role slot stamped startTime=-1 is judged Unknown =
    // never steal, so the role could never be taken over after a crash.
    ThrowError(isolate, "E_SYSTEM",
               "cannot establish thread identity (/proc unavailable); refusing to claim a ring role",
               name);
  }
  auto* roleWord = AtomicWord(data + roleWordOffset);
  // Bound the scan (review F23): a stuck slot must fail with E_TIMEOUT, never
  // spin this loop at 100% CPU with the JS thread blocked.
  for (int guard = 0; guard < 10000; guard++) {
    const uint32_t cur = static_cast<uint32_t>(roleWord->load(std::memory_order_acquire));
    if (cur != 0) {
      // Same-thread re-claim (review F23): the role is OURS — a second
      // RingProducer.open on this thread gets its own token back instead of
      // E_ROLE_TAKEN against itself.
      const uint32_t curSlot = (cur & kMutexTokenMask) >> 16;
      if (curSlot == slotIndex && SlotState(data, slotsBase, slotIndex) == kMutexStateActive) {
        const Identity id = SlotIdentity(data, slotsBase, slotIndex);
        if (id.pid == self.pid && id.threadId == self.threadId &&
            id.startTime == self.startTime) {
          if (isProducer) return cur;
          ThrowError(isolate, "E_ROLE_TAKEN",
                     "ring consumer already open on this thread (two live consumer "
                     "instances could double-process a message)", name);
        }
      }
      // A holder exists: live -> E_ROLE_TAKEN; dead -> clear and take over.
      if (MutexOwnerAlive(data, cur, slotsBase, 2)) {
        ThrowError(isolate, "E_ROLE_TAKEN",
                   "ring role already held by a live participant", name);
      }
      int32_t expected = static_cast<int32_t>(cur);
      if (!roleWord->compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        continue;  // raced with another takeover: re-read
      }
    }

    // Claim our slot: reuse this thread's live slot, take a free one (the pid
    // word is the publish-claim marker, review F23), or take over a dead
    // holder's slot.
    const uint32_t st = SlotState(data, slotsBase, slotIndex);
    if (st == kMutexStateActive) {
      const Identity id = SlotIdentity(data, slotsBase, slotIndex);
      if (id.pid == self.pid && id.threadId == self.threadId &&
          id.startTime == self.startTime) {
        // our own slot from a previous claim in this thread
        const uint32_t token =
            (slotIndex << 16) | (SlotGen(data, slotsBase, slotIndex) & kMutexGenMask);
        int32_t expected2 = static_cast<int32_t>(cur);
        if (roleWord->compare_exchange_strong(expected2, static_cast<int32_t>(token),
                                              std::memory_order_acq_rel)) {
          return token;
        }
        continue;
      }
      if (CheckLiveness(id) != Liveness::kDead) continue;
      // Dead Active role holder: rewrite the identity in place. PublishClaimedSlot
      // wins the gen CAS exactly once, so takeover racers cannot double-write;
      // a loser of the role-word CAS below recovers on its next scan (the slot
      // now reads as OUR active slot and re-claims through the mine-branch).
      uint32_t token = 0;
      if (!PublishClaimedSlot(data, slotsBase, slotIndex, self, &token)) continue;
      int32_t expected3 = static_cast<int32_t>(cur);
      if (roleWord->compare_exchange_strong(expected3, static_cast<int32_t>(token),
                                            std::memory_order_acq_rel)) {
        MutexTrackRole(isolate, data, token, static_cast<int>(slotIndex), roleWordOffset);
        return token;
      }
      continue;  // role word raced: next scan re-claims through the mine-branch
    }
    if (st == kMutexStateFree) {
      auto* pidWord = AtomicWord(SlotPtr(data, slotsBase, slotIndex, kSlotPid));
      const int32_t pidNow = pidWord->load(std::memory_order_acquire);
      int32_t expectedPid = pidNow;
      if (pidNow == 0) {
        if (!pidWord->compare_exchange_strong(expectedPid, static_cast<int32_t>(self.pid),
                                              std::memory_order_acq_rel)) {
          continue;
        }
      } else if (pidNow != static_cast<int32_t>(self.pid)) {
        // mid-publish by a dead claimer: recover it (review F23)
        if (CheckPidAlive(pidNow) != Liveness::kDead) continue;
        if (!pidWord->compare_exchange_strong(expectedPid, static_cast<int32_t>(self.pid),
                                              std::memory_order_acq_rel)) {
          continue;
        }
      }
      // PublishClaimedSlot bumps gen skipping token-0 values (review F12: the
      // ring's role words used to hit token 0 = "free" every 32768th claim).
      uint32_t token = 0;
      if (!PublishClaimedSlot(data, slotsBase, slotIndex, self, &token)) {
        // lost the gen race: give the publish right back and rescan
        pidWord->store(0, std::memory_order_release);
        continue;
      }
      int32_t expected3 = static_cast<int32_t>(cur);
      if (roleWord->compare_exchange_strong(expected3, static_cast<int32_t>(token),
                                            std::memory_order_acq_rel)) {
        // the role dies with the isolate: track for the §7.1 env-cleanup hook
        MutexTrackRole(isolate, data, token, static_cast<int>(slotIndex), roleWordOffset);
        return token;
      }
      // Lost the role CAS (raced takeover): drop our slot claim and retry.
      pidWord->store(0, std::memory_order_release);
      CasSlotState(data, slotsBase, slotIndex, kMutexStateActive, kMutexStateFree);
      continue;
    }
  }
  ThrowError(isolate, "E_TIMEOUT", "ring role claim made no progress (slot stuck?)",
             name);
}

}  // namespace

void MutexTrackRole(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot,
                    uint32_t roleWordOffset) {
  std::lock_guard<std::mutex> lock(g_held_mu);
  HeldState& st = g_held[isolate];
  const std::shared_ptr<Mapping> pin = Registry::FindByAddress(data);
  if (!st.hookRegistered) {
    st.hookRegistered = true;
    node::AddEnvironmentCleanupHook(
        isolate,
        [](void* p) { MutexCleanupIsolate(static_cast<v8::Isolate*>(p)); },
        isolate);
  }
  st.locks.push_back({data, token, slot, true, roleWordOffset, nullptr, std::move(pin)});
}

uint32_t RingClaimRole(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t roleWord, uint32_t slotIndex, bool isProducer) {
  return ClaimRoleSlot(isolate, name, data, kRingSlotsWordOffset, roleWord, slotIndex,
                       isProducer);
}

}  // namespace membridge
