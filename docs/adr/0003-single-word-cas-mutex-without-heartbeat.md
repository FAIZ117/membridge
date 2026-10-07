# 0003. Crash-safe Mutex: single-word CAS, no heartbeat

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (per PLAN rev 2)

## Context

A cross-process mutex must survive holder death. shmbuf had no mutex. Rev 1
of the plan used a heartbeat + pid check and kept state/owner/generation in
separate words — the heartbeat steals from live owners stuck in synchronous
code (the event loop is blocked, so a holder cannot pet the dog), pid reuse
makes pid-only checks wrong (F9), and separate words made stealing non-atomic.

## Decision

One 32-bit token word: `0 = free`, otherwise `(slotIndex << 16) | gen15`
with bit 31 = HAS_WAITERS. Lock is CAS(0 → myToken); unlock is verify + store
0 + notify; stealing a dead holder is CAS(deadToken → myToken). The token
points at a participant slot whose identity (pid + start time + pid-ns inode
+ thread id, §7.1) was written before the CAS, so a holder that crashes
between "acquire" and "record owner" cannot exist. Slot reclamation bumps the
slot's generation, instantly invalidating every outstanding token pointing
there. A per-isolate env-cleanup hook releases a terminated worker's locks as
OWNER_DIED; `ownerDied` is reported to the next holder (pthread
EOWNERDEAD analogue) because only the caller can repair half-updated data.
No heartbeat. No priority inheritance. Re-entrancy is E_DEADLOCK.

## Alternatives considered

- **OS robust mutexes** (PTHREAD_MUTEX_ROBUST / WAIT_ABANDONED): macOS has no
  robust mutexes; OS mutexes must be released by the locking thread, which
  conflicts with lockAsync (wait on another thread); glibc owns the robust
  list, so our futex words cannot join it.
- **Heartbeat:** steals from live, busy owners — the failure it was meant to
  prevent.
- **Atomics.wait-based waits:** F1 — polling across processes.

## Consequences

Deadlock detection is bounded by the 250 ms wait-slice liveness check, not
instant. A dead holder in a foreign pid namespace is treated as alive forever
(safe; recovery needs a process in the owner's namespace or reap). Up to 64
concurrent participant threads per mutex; exhausted tables surface as
E_TIMEOUT after reclamation finds nothing dead.

## Amendment (2026-10-07, fix round 3)

The slot state machine drops its RESERVED state (review F23). The claimer's
**pid word doubles as the publish-claim marker**: a virgin slot reads pid 0, a
slot mid-publish reads the reserver's pid while the state word is still Free,
and release zeroes the pid before the Free CAS — so `Free ⇒ pid 0 or a crashed
reserver`. A mid-publish slot whose pid is provably dead is recovered by CASing
the pid to the claimer's; previously a crash inside the publish window leaked
the slot forever (the claim loop only skipped it). Exclusivity of the publish
right moved entirely to a single-attempt gen CAS: the old RESERVED state-CAS
was doing that job, and a retrying gen bump would have let two racers publish
two tokens for one slot. A recycled pid reads alive, leaving one slot unusable
until process exit — the same accepted liveness-at-read-time risk the Active
reclaim carries. Ring roles share the machine, and a collected consumer
releases its role (R15b) so a replacement can open on the same thread.

## Amendment (2026-10-07, round 3) — superseded by ADR 0007

The pid-marker slot machine above lost mutual exclusion: threads of one process
share a pid (C2), the publish re-read gen after the liveness verdict (C3), and a
single native claim entry served every instance on a thread (C1). ADR 0007
replaces it: the state word carries an exclusive Claiming right (`pid << 2 | 2`),
claims are serialized per process, and each JS instance holds its own claim
reference.
