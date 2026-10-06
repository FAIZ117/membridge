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

uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name) {
  const uint32_t slotsBase = kMutexHeaderWords;
  const Identity self = SelfIdentity();

  // 1) Reuse this thread's existing slot (per-thread participant slot, §7.2).
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) == kMutexStateActive &&
        AtomicWord(SlotPtr(data, slotsBase, s, kSlotPid))->load() == self.pid &&
        AtomicWord(SlotPtr(data, slotsBase, s, kSlotTid))->load() == self.threadId &&
        SlotIdentity(data, slotsBase, s).startTime == self.startTime) {
      return (s << 16) | (SlotGen(data, slotsBase, s) & kMutexGenMask);
    }
  }

  // 2) Free slot, or reclaim one whose identity is provably dead and whose
  //    token is not the current lockWord value (§7.2).
  const uint32_t currentLock = static_cast<uint32_t>(
      AtomicWord(data)->load(std::memory_order_acquire) & static_cast<int32_t>(kMutexTokenMask));
  for (uint32_t pass = 0; pass < 2; pass++) {
    for (uint32_t s = 0; s < kMutexSlotCount; s++) {
      const uint32_t st = SlotState(data, slotsBase, s);
      if (st == kMutexStateReserved) continue;
      if (st == kMutexStateActive) {
        // candidate only when provably dead AND unreferenced by the lock word
        const uint32_t staleToken = (s << 16) | (SlotGen(data, slotsBase, s) & kMutexGenMask);
        if (staleToken == currentLock) continue;
        if (CheckLiveness(SlotIdentity(data, slotsBase, s)) != Liveness::kDead) continue;
      } else if (st != kMutexStateFree) {
        continue;
      }
      // Reserve, then bump gen (invalidates every outstanding token), then
      // publish identity + ACTIVE. The gen CAS cannot fail while we hold the
      // reservation: gen changes only under it.
      if (!CasSlotState(data, slotsBase, s, st, kMutexStateReserved)) continue;
      uint32_t g = SlotGen(data, slotsBase, s);
      int32_t gExpected = static_cast<int32_t>(g);
      while (!AtomicWord(SlotPtr(data, slotsBase, s, kSlotGen))
                  ->compare_exchange_strong(gExpected, static_cast<int32_t>(g + 1),
                                            std::memory_order_acq_rel)) {
        g = static_cast<uint32_t>(gExpected);
      }
      int32_t* idw = SlotPtr(data, slotsBase, s, 0);
      AtomicWord(idw + kSlotPid)->store(self.pid, std::memory_order_release);
      AtomicWord(idw + kSlotTid)->store(self.threadId, std::memory_order_release);
      AtomicWord(idw + kSlotStartLo)->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) & 0xFFFFFFFFu));
      AtomicWord(idw + kSlotStartHi)->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) >> 32));
      AtomicWord(idw + kSlotNsLo)->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) & 0xFFFFFFFFu));
      AtomicWord(idw + kSlotNsHi)->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) >> 32));
      AtomicWord(SlotPtr(data, slotsBase, s, kSlotState))
          ->store(static_cast<int32_t>(kMutexStateActive), std::memory_order_release);
      return (s << 16) | ((g + 1) & kMutexGenMask);
    }
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
  if (slot >= slotCount) return true;  // malformed: never steal
  if (SlotState(data, slotsWordOffset, slot) != kMutexStateActive) return true;
  if ((SlotGen(data, slotsWordOffset, slot) & kMutexGenMask) != gen) return true;  // stale
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
  // keeps the pages mapped until the hook runs — the SAB may be GC'd first
  std::shared_ptr<Mapping> pin;
};

struct HeldState {
  std::vector<HeldLock> locks;
  bool hookRegistered = false;
};

std::mutex g_held_mu;
std::map<v8::Isolate*, HeldState> g_held;

}  // namespace

void MutexTrackHeld(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot) {
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
  for (const HeldLock& h : st.locks) {
    if (h.data == data && h.token == token) return;
  }
  st.locks.push_back({data, token, slot, false, 0, std::move(pin)});
}

void MutexUntrackHeld(v8::Isolate* isolate, int32_t* data, uint32_t token) {
  std::lock_guard<std::mutex> lock(g_held_mu);
  auto it = g_held.find(isolate);
  if (it == g_held.end()) return;
  for (auto vit = it->second.locks.begin(); vit != it->second.locks.end(); ++vit) {
    if (vit->data == data && vit->token == token) {
      it->second.locks.erase(vit);
      return;
    }
  }
}

void MutexReleaseThreadSlots(int32_t* data) {
  const uint32_t slotsBase = kMutexHeaderWords;
  const Identity self = SelfIdentity();
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, slotsBase, s) == kMutexStateActive &&
        SlotIdentity(data, slotsBase, s).pid == self.pid &&
        SlotIdentity(data, slotsBase, s).threadId == self.threadId) {
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
  CasSlotState(h.data, kMutexHeaderWords, static_cast<uint32_t>(h.slot), kMutexStateActive,
               kMutexStateFree);
}

}  // namespace

void MutexCleanupIsolate(v8::Isolate* isolate) {
  std::vector<HeldLock> held;
  {
    std::lock_guard<std::mutex> lock(g_held_mu);
    auto it = g_held.find(isolate);
    if (it == g_held.end()) return;
    held.swap(it->second.locks);
    g_held.erase(it);
  }
  for (const HeldLock& h : held) {
    if (h.pin == nullptr) continue;  // mapping already gone: nothing to clear
    if (h.isRole) {
      // §8 role claim of a dead thread: clear the role word (if still ours)
      // and free the slot so a new participant can take over.
      auto* roleWord = AtomicWord(h.data + h.roleWordOffset);
      int32_t cur = roleWord->load(std::memory_order_acquire);
      if (static_cast<uint32_t>(cur) == h.token) {
        roleWord->compare_exchange_strong(cur, 0, std::memory_order_acq_rel);
      }
      CasSlotState(h.data, kRingSlotsWordOffset, static_cast<uint32_t>(h.slot),
                   kMutexStateActive, kMutexStateFree);
    } else {
      ReleaseHeldLock(h);
    }
  }
}

// ---- §8 ring role claims ----------------------------------------------------

namespace {

// A role claim is a restricted slot claim: the role owns exactly one slot.
uint32_t ClaimRoleSlot(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t slotsBase, uint32_t roleWordOffset, uint32_t slotIndex) {
  const Identity self = SelfIdentity();
  auto* roleWord = AtomicWord(data + roleWordOffset);

  for (;;) {
    const uint32_t cur = static_cast<uint32_t>(roleWord->load(std::memory_order_acquire));
    if (cur != 0) {
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

    // Claim our slot: reuse this thread's live slot, else the role slot is
    // free (never used or freed) or its last owner is dead -> reclaim.
    const uint32_t st = SlotState(data, slotsBase, slotIndex);
    bool usable = false;
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
      usable = CheckLiveness(id) == Liveness::kDead;
    } else if (st == kMutexStateFree) {
      usable = true;
    }
    if (!usable || !CasSlotState(data, slotsBase, slotIndex, st, kMutexStateReserved)) {
      continue;  // raced: retry the whole claim
    }
    uint32_t g = SlotGen(data, slotsBase, slotIndex);
    int32_t gExpected = static_cast<int32_t>(g);
    while (!AtomicWord(SlotPtr(data, slotsBase, slotIndex, kSlotGen))
                ->compare_exchange_strong(gExpected, static_cast<int32_t>(g + 1),
                                          std::memory_order_acq_rel)) {
      g = static_cast<uint32_t>(gExpected);
    }
    int32_t* idw = SlotPtr(data, slotsBase, slotIndex, 0);
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
    AtomicWord(SlotPtr(data, slotsBase, slotIndex, kSlotState))
        ->store(static_cast<int32_t>(kMutexStateActive), std::memory_order_release);
    const uint32_t token = (slotIndex << 16) | ((g + 1) & kMutexGenMask);
    int32_t expected3 = static_cast<int32_t>(cur);
    if (roleWord->compare_exchange_strong(expected3, static_cast<int32_t>(token),
                                          std::memory_order_acq_rel)) {
      // the role dies with the isolate: track for the §7.1 env-cleanup hook
      MutexTrackRole(isolate, data, token, static_cast<int>(slotIndex), roleWordOffset);
      return token;
    }
    // Lost the role CAS (raced takeover): drop our slot claim and retry.
    CasSlotState(data, slotsBase, slotIndex, kMutexStateActive, kMutexStateFree);
  }
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
  st.locks.push_back({data, token, slot, true, roleWordOffset, std::move(pin)});
}

uint32_t RingClaimRole(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t roleWord, uint32_t slotIndex) {
  return ClaimRoleSlot(isolate, name, data, kRingSlotsWordOffset, roleWord, slotIndex);
}

}  // namespace membridge
