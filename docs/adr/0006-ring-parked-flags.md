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
advancing its counter and notifies only when set. On Linux ≥5.16 the parked
wait usually rides the futex_waitv multiplexer.

## Alternatives considered

- **Keep unconditional wakes:** simplest, but dominates small-message cost.
- **Exact waiter counts:** more state to crash-recover; a boolean is enough
  because extra wakes are already allowed (spurious-wake model, §6).

## Consequences

A waiter that crashes between set and clear leaves the flag set — the other
side then sends one futile wake per operation (bounded, harmless). The
layout change uses only previously-padding words and keeps layoutVersion 1
(nothing was ever published). PLAN §8.1 documents the words.
