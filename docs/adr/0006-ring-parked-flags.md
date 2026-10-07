# 0006. RingBuffer parked flags: notify only when someone is parked

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (fix round for the pre-release review)

## Context

commit()/release() called sync.notify unconditionally: one FUTEX_WAKE per
message per side even with no waiter — ~0.16 µs against a 220–450 ns
small-message budget (review Perf P3; measured +40–80% msgs/s with flags).

## Decision

Two flag words in existing padding (word 1 shares head's cache line, word 17
tail's): CONSUMER_PARKED and PRODUCER_PARKED. The waiting side stores 1,
RE-CHECKS its condition (JS Atomics are sequentially consistent, so
flag-then-recheck is race-free), parks, and clears the flag on exit
(including the found-data path). The notifying side loads the flag after
advancing its counter and notifies only when set.
**[Amended 2026-10-07: the parked wait does NOT ride the futex_waitv
multiplexer — sync paths futex directly on the JS thread and are woken by the
peer's direct FUTEX_WAKE; async paths ride the mux (or the thread fallback).
The original sentence described a path that does not exist.]**

## Alternatives considered

- **Keep unconditional wakes:** simplest, but dominates small-message cost.
- **Exact waiter counts:** more state to crash-recover; a boolean is enough
  because extra wakes are already allowed (spurious-wake model, §6).

## Amendment (2026-10-07, fix round 2)

The flags are set/cleared by the **synchronous** `reserve`/`commit`/`peek`/
`release` paths only. The async variants (`reserveAsync`/`peekAsync`) initially
waited without touching the flags, so their peers never notified them and every
async wake cost a full wait slice (review R14: async consumer 383 msgs/s with
37% of messages over 200 ms). **Fixed in fix round 2 after the re-verification
confirmed the first fix had missed the async paths entirely** — they now follow
the same set/re-check/clear pattern around their `waitAsync` waits, and a
latency regression test asserts a parked `peekAsync` wakes in milliseconds
when the producer commits.
Also: a crashed waiter leaves its flag set — a busy successor that never parks
then pays one futile wake per message until it parks once.

**Correction (round 3, 2026-10-07):** the sentence that used to end this
paragraph — "role claims reset the peer's flags at takeover" — described code
that never landed (re-verification C13). Round 3 implements it: a fresh role
holder clears its OWN parked flag when it claims the role. The async re-check
in that fix also compared the counter with itself (C6); waiters now snapshot
the counter before their non-blocking attempt and re-check against the
snapshot.

**Plain load, not exchange (round 3 C20, tried and reverted).** Clearing the
peer's flag with an exchange-to-0 when notifying looks like it caps a full ring
at one wake per park, but it loses wakes: a notifier's flag check is not tied to
the counter value it published, so a late exchange from an EARLIER commit can
consume the peer's NEW park announcement — its wake spent before the peer
slept — and the next commit then sees the flag clear and wakes nobody (a full
250 ms slice; reproduced by `test/round3.test.ts`, 2–5 stalls per 8×20k
messages). Notifiers therefore load the flag; the waiter alone clears it. The
accepted cost is roughly one wake per release while the peer is parked.

## Consequences

A waiter that crashes between set and clear leaves the flag set — the other
side then sends one futile wake per operation (bounded, harmless). The
layout change uses only previously-padding words and keeps layoutVersion 1
(nothing was ever published). PLAN §8.1 documents the words.
