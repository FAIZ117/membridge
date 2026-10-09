# 0002. Cross-process wait/notify over OS primitives, not Atomics

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (per PLAN rev 2)

## Context

`Atomics.notify` does not wake waiters in another process — V8 keeps its
waiter list per process (PLAN §2 F1; probes/f1-cross-process-notify.js). Any
lock or queue built on `Atomics.wait` degrades to polling at the timeout
interval. The spike (probes/f14-shared-futex.js) proved Linux shared futexes
(FUTEX_WAIT/FUTEX_WAKE without FUTEX_PRIVATE_FLAG) wake across processes on
shm pages, and that `futex_waitv` (kernel ≥ 5.16, raw syscall — glibc has no
wrapper, F14) returns the **0-based index of the woken entry**, which is what
makes a single-thread multiplexer possible.

## Decision

`shm-bridge/sync` exposes wait/waitAsync/notify over i32 words. Linux uses raw
shared-futex syscalls; `waitAsync` runs on shm-bridge-owned threads (never the
libuv pool), multiplexed through one `futex_waitv` thread (127 waits + a
process-private control word) with a one-thread-per-wait fallback below
kernel 5.16 or a full cap (E_TOO_MANY_WAITERS). macOS uses `os_sync`
SHARED when available (U1), else bounded poll; Windows uses per-word named
semaphores (U3). Fulfilment crosses to the isolate's loop thread via uv_async
+ microtask checkpoint; the event loop is pinned from JS while waits are
outstanding (uv_ref alone proved unreliable on the Node 22 runner). Isolate
teardown cancels that isolate's waits and resolves 'timed-out'.

## Alternatives considered

- **Atomics + short timeouts (poll):** F1 makes every handoff pay the poll
  interval; correctness fine, latency poor.
- **libuv threadpool for waits:** four threads total; a few lock waits would
  starve fs/dns/crypto for the whole process.
- **One OS thread per wait always:** unbounded thread growth under load;
  `futex_waitv` was cheap to adopt once the return semantics were pinned.

## Consequences

Waitable words are i32 only across all layouts. Wake latency on Linux is a
futex wake (~µs); on macOS/Windows it depends on U1/U3 outcomes, with the
poll fallback bounded at 2 ms. A missed wake costs latency, never
correctness: callers re-check the word on every wake and timeout.
