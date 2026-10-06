# 0005. Mutex wake protocol preserves HAS_WAITERS across acquisition

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (fix round for the pre-release review)

## Context

The original §7.3 protocol acquired with `CAS(word, 0, token)` — a whole-word
write that cleared HAS_WAITERS — and unlocked with `notify` only when the bit
was set. Two lost-wakeup paths fell out (review Corr F10 / Perf P2, measured
14.2k vs ~40k acquisitions/s with 3 processes, and handoffs paying a full
250 ms slice):

1. A waiter ORs the bit into a word that is already free (the holder
   unlocked between the waiter's load and its OR), then sleeps on the
   free-with-bit word; later unlocks see no owner... but the bit IS set, so
   they notify — except the acquiring CAS path (2) keeps erasing the bit.
2. A woken waiter acquires with a bare token, erasing HAS_WAITERS while
   OTHER waiters are still parked; its eventual unlock sees no bit and never
   wakes them. They burn full wait slices while the lock is free.

This is the classic futex-mutex defect Drepper's "mutex2" paper describes.

## Decision

Acquisition is still a single CAS, but the written word preserves the
HAS_WAITERS bit once the acquirer has itself parked at least once
(`token | HAS_WAITERS`); a never-waiting acquirer writes a bare token (and so
clears any stale bit a lost race left behind). Waiters, after setting the
bit, RE-READ the word and retry immediately when it reads free instead of
parking on it. `tryLock` preserves the bit on acquire. Unlock semantics are
unchanged: notify one when the bit is set. The bit is intentionally never
cleared by an unlock — the worst case is one extra FUTEX_WAKE (~0.16 µs)
per unlock in a system that HAD waiters, which we accept.
**[Amended 2026-10-07: unlock's whole-word CAS to 0 DOES clear the bit, and
the waiter path always ORs — both corrections below supersede this
paragraph.]**

## Alternatives considered

- **Kernel-managed waiters (FUTEX_LOCK_PI):** requires the kernel's TID-word
  format, conflicts with the slot-token design, absent on macOS/Windows.
- **Track an exact waiter count in a second word:** two words to keep
  consistent under races; the bit alone is sufficient once acquisition
  preserves it.

## Amendment (2026-10-07, fix round 2)

The waiter path must issue `or(lockWord, HAS_WAITERS)` on **every** loop pass,
not only when the word it read lacked the bit. Skipping the OR when the first
read already showed HAS_WAITERS left a race: a never-waited newcomer acquires
with a bare token between the waiter's read and park, erasing the bit; the
waiter then parks on `expected` without the bit, the newcomer's unlock sees no
bit and does not notify, and the waiter sleeps a full slice. Re-verification
measured 32 slice timeouts in 4 s under tight loops before the fix. The parked
value is always `prev | HAS_WAITERS` (the OR's return word plus the bit), and
acquisition after parking writes `token | HAS_WAITERS`. Also amended: unlock
*does* clear the bit via its whole-word CAS to 0 — the consequence paragraph
below originally claimed otherwise (harmless: the waiter re-ORs on its next
pass, costing at most one extra wake).

## Consequences

Handoffs after a wake no longer depend on the timeout slice (measured:
no slice timeouts, max wait ms-scale instead of 750 ms). Unlocks after
contention pay one extra wake. Deadlock detection cadence (250 ms liveness
slices) is unchanged, per PLAN §7.3.
