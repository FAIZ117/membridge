# 0007. Slot claims: an exclusive state-word right, process-wide serialization, per-instance references

- **Status:** accepted (supersedes the "Slot states" amendment of ADR 0003)
- **Date:** 2026-10-07
- **Deciders:** Faiz (round-3 fix round)

## Context

The round-3 re-verification (`docs/review/2026-10-07T1236-round3-reverification.md`)
broke mutual exclusion three ways, each reproduced through the public API:

- **C1** — two `Mutex` instances on one thread share one slot and token, but
  the native side kept ONE claim entry per (data, slot). Closing or garbage-
  collecting either instance released the lock the *other* one held.
- **C2** — the F23 scheme used the slot's pid word as the publish-claim marker.
  Threads share a pid, so a second worker read "my own pid" and published into
  the same slot; the publish re-read gen fresh, so its gen CAS also won.
- **C3** — a reclaimer read gen, judged the identity dead, then published by
  re-reading gen: a reclaim that completed in between was overwritten.
- **C4** — ring role claims had the same thread race and a loser path that freed
  the winner's slot.

## Decision

1. **The state word is the publish right.** `0` Free, `1` Active,
   `(pid << 2) | 2` Claiming. A claimer wins the right with one CAS from the
   state it observed — Free, a Claiming word whose pid is provably dead, or
   (reclaim) Active — and a reclaim then re-checks that gen still equals the
   value read *before* the liveness verdict, backing off otherwise. Only the
   holder of the right writes gen and identity; readers never judge a Claiming
   slot, and a dead verdict counts only if state and gen are unchanged after it.
2. **Claims are serialized per process** by a native mutex. No shared word can
   distinguish two threads of one process; a process-local lock can, at no
   cross-process cost.
3. **One claim reference per JS instance.** `mutexClaimSlot` claims (or reuses)
   the thread's slot and adds a reference pinned by the instance's own SAB.
   Dropping a reference releases the lock only when that instance held it
   (`wasHeld`, carried by the FinalizationRegistry's held value); the last
   reference frees the slot. Ring roles use the same references; a second live
   producer on a thread is the same JS instance (interned), a second consumer is
   `E_ROLE_TAKEN`.

## Alternatives considered

- **pid+tid marker in the state word:** does not fit in 32 bits, and two words
  cannot be CASed together.
- **Re-introduce RESERVED with a separate owner word:** a crash between the two
  writes is unrecoverable without knowing the reserver — the original F23 bug.
- **Intern Mutex instances per thread:** would share hold state across
  independent call sites (one site's `close()` would close the other's handle).
  Per-instance references keep handles independent.

## Consequences

- Mutual exclusion holds for concurrent claims across threads and processes
  (`test/round3.test.ts` C2/C3/C4: 0 overlaps; the same tests overlap or hang at
  `248f731`).
- Pid values must fit in 30 bits (Linux `pid_max` ≤ 2^22; Windows pids are far
  below 2^30 in practice).
- A crashed claimer's Claiming word is recovered by a pid-only liveness check; a
  recycled pid reads alive and leaves that slot unusable until it dies — the same
  accepted liveness-at-read-time risk as before.
- A role claim that meets a live participant mid-claim waits up to 100 ms for
  it to settle before `E_ROLE_TAKEN` (added after verification). `g_claim_mu`
  is held meanwhile — it delays only other claims in this process.
- No cost on the lock/unlock hot path (claims happen once per instance):
  uncontended lock+unlock 49–51 ns vs 48–59 ns at `248f731` (same session).
