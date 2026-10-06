// mutex.cc — slot claims, owner liveness, and the §7.1 env-cleanup hook.

#include "mutex.h"
#include "liveness.h"
#include "wait.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include <string.h>

namespace membridge {

void MutexCleanupIsolate(v8::Isolate* isolate);

namespace {

inline std::atomic<int32_t>* AtomicWord(int32_t* p) {
  return reinterpret_cast<std::atomic<int32_t>*>(p);
}

inline int32_t* SlotPtr(int32_t* data, uint32_t slot, uint32_t word) {
  return data + kMutexHeaderWords + slot * kMutexSlotWords + word;
}

// identity words inside a slot: pid@0, tid@1, startLo@2, startHi@3, nsLo@4, nsHi@5; gen@6, state@7
constexpr uint32_t kSlotPid = 0, kSlotTid = 1, kSlotStartLo = 2, kSlotStartHi = 3,
                   kSlotNsLo = 4, kSlotNsHi = 5, kSlotGen = 6, kSlotState = 7;

Identity SlotIdentity(int32_t* data, uint32_t slot) {
  int32_t* s = SlotPtr(data, slot, 0);
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

uint32_t SlotGen(int32_t* data, uint32_t slot) {
  return static_cast<uint32_t>(
      AtomicWord(SlotPtr(data, slot, kSlotGen))->load(std::memory_order_acquire));
}

uint32_t SlotState(int32_t* data, uint32_t slot) {
  return static_cast<uint32_t>(
      AtomicWord(SlotPtr(data, slot, kSlotState))->load(std::memory_order_acquire));
}

bool CasSlotState(int32_t* data, uint32_t slot, uint32_t expect, uint32_t next) {
  int32_t* p = SlotPtr(data, slot, kSlotState);
  int32_t exp = static_cast<int32_t>(expect);
  return AtomicWord(p)->compare_exchange_strong(exp, static_cast<int32_t>(next),
                                                std::memory_order_acq_rel);
}

}  // namespace

uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name) {
  const Identity self = SelfIdentity();

  // 1) Reuse this thread's existing slot (per-thread participant slot, §7.2).
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, s) == kMutexStateActive &&
        AtomicWord(SlotPtr(data, s, kSlotPid))->load() == self.pid &&
        AtomicWord(SlotPtr(data, s, kSlotTid))->load() == self.threadId &&
        SlotIdentity(data, s).startTime == self.startTime) {
      return (s << 16) | (SlotGen(data, s) & kMutexGenMask);
    }
  }

  // 2) Free slot, or reclaim one whose identity is provably dead and whose
  //    token is not the current lockWord value (§7.2).
  const uint32_t currentLock = static_cast<uint32_t>(
      AtomicWord(data)->load(std::memory_order_acquire) & static_cast<int32_t>(kMutexTokenMask));
  for (uint32_t pass = 0; pass < 2; pass++) {
    for (uint32_t s = 0; s < kMutexSlotCount; s++) {
      const uint32_t st = SlotState(data, s);
      if (st == kMutexStateReserved) continue;
      if (st == kMutexStateActive) {
        // candidate only when provably dead AND unreferenced by the lock word
        const uint32_t staleToken = (s << 16) | (SlotGen(data, s) & kMutexGenMask);
        if (staleToken == currentLock) continue;
        if (CheckLiveness(SlotIdentity(data, s)) != Liveness::kDead) continue;
      } else if (st != kMutexStateFree) {
        continue;
      }
      // Reserve, then bump gen (invalidates every outstanding token), then
      // publish identity + ACTIVE. The gen CAS cannot fail while we hold the
      // reservation: gen changes only under it.
      if (!CasSlotState(data, s, st, kMutexStateReserved)) continue;
      uint32_t g = SlotGen(data, s);
      int32_t gExpected = static_cast<int32_t>(g);
      while (!AtomicWord(SlotPtr(data, s, kSlotGen))
                  ->compare_exchange_strong(gExpected, static_cast<int32_t>(g + 1),
                                            std::memory_order_acq_rel)) {
        g = static_cast<uint32_t>(gExpected);
      }
      int32_t* idw = SlotPtr(data, s, 0);
      AtomicWord(idw + kSlotPid)->store(self.pid, std::memory_order_release);
      AtomicWord(idw + kSlotTid)->store(self.threadId, std::memory_order_release);
      AtomicWord(idw + kSlotStartLo)->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) & 0xFFFFFFFFu));
      AtomicWord(idw + kSlotStartHi)->store(static_cast<int32_t>(static_cast<uint64_t>(self.startTime) >> 32));
      AtomicWord(idw + kSlotNsLo)->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) & 0xFFFFFFFFu));
      AtomicWord(idw + kSlotNsHi)->store(static_cast<int32_t>(static_cast<uint64_t>(self.pidNsInode) >> 32));
      AtomicWord(SlotPtr(data, s, kSlotState))
          ->store(static_cast<int32_t>(kMutexStateActive), std::memory_order_release);
      return (s << 16) | ((g + 1) & kMutexGenMask);
    }
  }
  ThrowError(isolate, "E_TIMEOUT",
             "no mutex participant slot available for thread (all " +
                 std::to_string(kMutexSlotCount) + " slots held by live threads)",
             name);
}

bool MutexOwnerAlive(int32_t* data, uint32_t token) {
  const uint32_t slot = (token & kMutexTokenMask) >> 16;
  const uint32_t gen = token & kMutexGenMask;
  if (slot >= kMutexSlotCount) return true;  // malformed: never steal
  if (SlotState(data, slot) != kMutexStateActive) return true;
  if ((SlotGen(data, slot) & kMutexGenMask) != gen) return true;  // stale token
  const Liveness l = CheckLiveness(SlotIdentity(data, slot));
  return l != Liveness::kDead;  // unknown liveness -> alive (never steal, §7.1)
}

// ---- held-lock registry for the env-cleanup hook ---------------------------

namespace {

struct HeldLock {
  int32_t* data;
  uint32_t token;
  int slot;
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
  st.locks.push_back({data, token, slot});
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
  const Identity self = SelfIdentity();
  for (uint32_t s = 0; s < kMutexSlotCount; s++) {
    if (SlotState(data, s) == kMutexStateActive && SlotIdentity(data, s).pid == self.pid &&
        SlotIdentity(data, s).threadId == self.threadId) {
      CasSlotState(data, s, kMutexStateActive, kMutexStateFree);
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
  CasSlotState(h.data, static_cast<uint32_t>(h.slot), kMutexStateActive, kMutexStateFree);
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
  for (const HeldLock& h : held) ReleaseHeldLock(h);
}

}  // namespace membridge
