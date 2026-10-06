# Benchmarks

Informational numbers from `npm run bench` (never a CI gate — PLAN §11/F10:
shared runners are noisy, and throughput thresholds in tests rot). Machine for
the numbers below: dev workstation, Linux 7.2.8 x86_64 (Fedora), Node 24.18.0,
g++ 15.3, Release build, 18 cores, otherwise idle. Re-run with `npm run bench`.

## Results (2026-10-06, after the review fix round)

| Bench | Value |
|-------|-------|
| contention — same-process `Atomics.add` (5 M ops on a membridge SAB) | ~110–120 M ops/s |
| contention — cross-process futex handoff (both sides parked, round trip) | ~0.006 ms/round-trip (~175k/s) |
| mutex — uncontended `lock`+`unlock` (100k iters, same thread) | **~0.07 µs/op** (was 0.18 before per-lock native tracking was removed) |
| mutex — 3-way contended handoff | sub-slice handoffs in the standard bench; under TIGHT loops a never-waited newcomer can still erase `HAS_WAITERS` once and cost one wait slice (ADR 0005 residual, bounded) |
| mutex — holder SIGKILLed → steal (SIGKILL → acquired) | ~0 ms (zombie-aware liveness) |
| ring — 64 B messages (copying `write`/`read`) | **~3.2 M msgs/s** (~205 MB/s; was ~2.3 M before parked flags) |
| ring — 4 KiB messages | ~330k msgs/s (~1.36 GB/s) |
| ring — 64 KiB messages | ~27k msgs/s (~1.8 GB/s) |
| ring — async (`reserveAsync`/`peekAsync`) with flags | millisecond-scale wake latency (R14 regression test asserts < 50 ms) |

The handoff number changed meaning in the fix round: the bench's pong side
now parks on the token instead of spinning, so it measures a true
sleep→wake→sleep round trip (~6 µs) rather than a spin-loop counterpart
(~2 µs against a busy peer).

## Reading them

- The `Atomics.add` line is the floor: pure user-space atomics over the shared
  page, no syscalls. Everything cross-process adds futex syscalls.
- The handoff line is two processes ping-ponging one word over shared futexes —
  the primitive §7/§8 blocking is built on. ~6 µs per round trip is the
  cross-process synchronization cost (the older ~2 µs figure was a spin-loop
  counterpart against a busy peer, not a sleep→wake round trip).
- Ring numbers use the **copying** convenience API (`write`/`read`) — one copy
  in and one copy out. The zero-copy `reserve`/`commit` + `peek`/`release` path
  avoids the payload copies entirely; these figures are the conservative bound.
- Mutex recovery is state-based, not timing-based: after a SIGKILL (and reap)
  the contender's next 250 ms wait-slice liveness check observes a dead owner
  and steals immediately (in practice the steal happens on the first
  liveness probe — ~0 ms in the bench, since the zombie state is already
  visible when the contender first checks).
