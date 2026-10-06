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

constexpr uint32_t kMutexStateFree = 0;
constexpr uint32_t kMutexStateActive = 1;
constexpr uint32_t kMutexStateReserved = 2;

// token = (slotIndex << 16) | gen15; bit 31 = HAS_WAITERS
constexpr uint32_t kMutexHasWaiters = 0x80000000u;
constexpr uint32_t kMutexTokenMask = 0x7FFFFFFFu;
constexpr uint32_t kMutexGenMask = 0x00007FFFu;

// Claim (or re-claim) a participant slot for the calling OS thread. Reuses
// this thread's live slot when one exists; otherwise takes a free slot or
// reclaims one whose identity fails the §7.1 liveness check and whose token
// is not the current lockWord value (gen bump invalidates stale tokens).
// Returns the token, or 0 when no slot is available (all 64 live).
uint32_t MutexClaimSlot(v8::Isolate* isolate, int32_t* data, const std::string& name);

// True when the token's owner slot exists, its gen matches, and its identity
// is alive (unknown liveness counts as alive — never steal across pid
// namespaces, §7.1). A gen mismatch (stale token) also reports alive: the
// real holder's unlock path fixes the word.
bool MutexOwnerAlive(int32_t* data, uint32_t token, uint32_t slotsWordOffset,
                     uint32_t slotCount);

// Per-isolate held-lock registry for the §7.1 env-cleanup hook: a dead worker
// must not leave locks behind in a live process. Tracking registers the hook
// on first use; teardown releases every still-held lock as OWNER_DIED and
// wakes one waiter.
void MutexTrackHeld(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot);
void MutexUntrackHeld(v8::Isolate* isolate, int32_t* data, uint32_t token);

// Free this thread's slot(s) in `data` (graceful close; slots of dead threads
// are reclaimed on demand anyway).
void MutexReleaseThreadSlots(int32_t* data);

// ---- §8 ring role claims ---------------------------------------------------
// The ring data region embeds a 2-slot participant table (same 8-word slot
// layout as the mutex) at kRingSlotsWordOffset; role word 0 = producer's
// token, 1 = consumer's. Claiming takes the role's own slot (producer -> 0,
// consumer -> 1): live holder -> E_ROLE_TAKEN; dead holder -> replaced with a
// gen bump; fresh segment -> plain claim.
constexpr uint32_t kRingSlotsWordOffset = 36;  // byte 144 (two 32 B slots)
constexpr uint32_t kRingProducerWord = 32;     // byte 128
constexpr uint32_t kRingConsumerWord = 33;     // byte 132

uint32_t RingClaimRole(v8::Isolate* isolate, const std::string& name, int32_t* data,
                       uint32_t roleWord, uint32_t slotIndex);

void MutexTrackRole(v8::Isolate* isolate, int32_t* data, uint32_t token, int slot,
                    uint32_t roleWordOffset);

}  // namespace membridge

#endif  // MEMBRIDGE_MUTEX_H_
