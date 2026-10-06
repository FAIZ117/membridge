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
