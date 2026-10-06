# 0004. Event-loop pinning for pending waits lives in JS

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (implementation finding, M5 stabilization)

## Context

A pending `waitAsync` must keep the event loop alive — otherwise Node exits
(or the test runner cancels the file) while promises are pending. The natural
native mechanism, `uv_ref` on the delivery handle, proved unreliable across
Node versions: on the Node 22 runner, roughly one run in three exited before
native waits settled even with the handle ref'd (and an unref'd handle with
manual ref/unref bookkeeping leaked both directions: loops that never
drained, and loop-drain cancellations).

## Decision

sync.ts pins the loop from JS: a ref'd `setInterval` is held while
`outstandingWaits > 0` and cleared when the count reaches zero — identical
semantics to a pending `setTimeout`. The native uv_async is a delivery-only,
permanently unref'd handle; it can wake a live loop but never holds one.
Every async-wait entry point (sync.waitAsync, Mutex.lockAsync) goes through
the pinned wrapper. The native side keeps an idempotent per-node delivery
guard (exactly one pending-decrement) and a thread-tail re-check.

## Alternatives considered

- **uv_ref/uv_unref bookkeeping in C++:** implemented first; flaky across
  Node majors (above), and each fix traded one failure mode for another.
- **Do nothing (document "keep a timer alive"):** pushes a native-module
  implementation detail onto every user.

## Consequences

A pending waitAsync behaves exactly like a pending setTimeout for process
lifetime purposes. The delivery handle can never delay process exit. Teardown
(worker terminate) closes the hub's uv handle so uv_loop_close cannot abort
on an open handle.
