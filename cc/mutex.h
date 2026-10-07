// mutex.h — §7 crash-safe mutex native support.
//
// The whole lock protocol is a single-CAS on the token word (§7.2), executed
// in JS with Atomics over the kind=mutex data region (which is user-visible
// memory). The native side provides what JS cannot do: the slot claim with
// liveness-based reclamation (§7.2), owner-liveness checks for the steal
// path (§7.1), and per-isolate tracking so the env-cleanup hook can release
// locks a dead worker still holds (§7.1).

#ifndef MEMBRIDGE_MUTEX_H_
#define MEMBRIDGE_MUTEX_H_

#include "membridge.h"

#include <cstdint>

namespace membridge {

// Data-region layout for kind=mutex (byte contract; mirrored in src/mutex.ts).
constexpr uint32_t kMutexSlotCount = 64;
constexpr uint32_t kMutexHeaderWords = 4;  // lockWord, ownerDied, seq, pad
constexpr uint32_t kMutexSlotWords = 8;    // identity 6 words + gen + state
constexpr uint32_t kMutexDataBytes =
    (kMutexHeaderWords + kMutexSlotCount * kMutexSlotWords) * 4;  // 2064

// Slot state word (round-3 fix C2/C3, PLAN §7.2):
//   0                  Free
//   1                  Active (identity + gen published)
//   (pid << 2) | 2     Claiming — the EXCLUSIVE publish right, held by `pid`.
// A claimer wins the right with ONE CAS from the state it observed (Free, a
// Claiming word whose pid is provably dead, or — for a reclaim — Active with
// the gen it read before its liveness verdict). Only the holder of the right
// writes gen and identity; readers never trust a Claiming slot's identity.
// Within one process all claims are serialized by a process-wide mutex, so
// two threads (which share a pid) can never both hold the right.
constexpr uint32_t kMutexStateFree = 0;
constexpr uint32_t kMutexStateActive = 1;
constexpr uint32_t kMutexStateClaimingTag = 2;  // low two bits of a Claiming word

// token = (slotIndex << 16) | gen15; bit 31 = HAS_WAITERS
constexpr uint32_t kMutexHasWaiters = 0x80000000u;
constexpr uint32_t kMutexTokenMask = 0x7FFFFFFFu;
constexpr uint32_t kMutexGenMask = 0x00007FFFu;

// Claim (or re-claim) a participant slot for the calling OS thread and
// register ONE claim reference for the calling JS instance (round-3 C1): the
// native entry is refcounted per (isolate, data, slot), and its pin is the
// SAB's own BackingStore (taken from the view — never a separate argument).
// Reuses this thread's live slot when one exists; otherwise takes a free slot
// or reclaims one whose identity fails the §7.1 liveness check and whose
// token is not the current lockWord value (gen bump invalidates stale tokens).
// Throws E_TIMEOUT when no slot is available (all 64 live).
uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name,
                        v8::Local<v8::SharedArrayBuffer> sab);

// True when the token's owner may still be alive (never steal). False only
// when the slot was reclaimed or freed since the token was issued, or its
// identity is provably dead. A slot mid-claim reports alive (retry later).
bool MutexOwnerAlive(int32_t* data, uint32_t token, uint32_t slotsWordOffset,
                     uint32_t slotCount);

// Drop ONE claim reference (a JS Mutex instance was collected or closed).
// `releaseHeld`: the dropped instance itself held the lock — release it as
// OWNER_DIED + wake (nobody can unlock it anymore). Only that instance's own
// hold is ever released (round-3 C1: a sibling instance on the same thread
// shares the token but not the hold). At refcount 0 the entry, its pin and
// this thread's slot are released.
void MutexUnregisterClaim(v8::Isolate* isolate, int32_t* data, int slot, bool releaseHeld);

// Free this thread's slot(s) in `data` (graceful close; slots of dead threads
// are reclaimed on demand anyway).
void MutexReleaseThreadSlots(int32_t* data);

// ---- §8 ring role claims ---------------------------------------------------
// The ring data region embeds a 2-slot participant table (same 8-word slot
// layout as the mutex) at kRingSlotsWordOffset; role word 0 = producer's
// token, 1 = consumer's. Claiming takes the role's own slot (producer -> 0,
// consumer -> 1): live holder -> E_ROLE_TAKEN; dead holder -> replaced with a
// gen bump; fresh segment -> plain claim. Same slot machine and process-wide
// serialization as the mutex. The role entry is refcounted per JS instance
// like a mutex claim; the role is released (word cleared, slot freed) when
// the last reference drops or the thread tears down.
constexpr uint32_t kRingSlotsWordOffset = 36;  // byte 144 (two 32 B slots)
constexpr uint32_t kRingProducerWord = 32;     // byte 128
constexpr uint32_t kRingConsumerWord = 33;     // byte 132
constexpr uint32_t kRingHeaderBytes = 256;

uint32_t RingClaimRole(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t roleWord, uint32_t slotIndex, bool isProducer,
                       v8::Local<v8::SharedArrayBuffer> sab);

// A ring instance was collected or close()d: drop one role reference; at 0
// clear the role word (if it still holds our token) and free the role slot.
void MutexUnregisterRole(v8::Isolate* isolate, int32_t* data, int slot);

}  // namespace membridge

#endif  // MEMBRIDGE_MUTEX_H_
