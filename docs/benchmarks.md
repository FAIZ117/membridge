# Benchmarks

Informational numbers from `npm run bench` (never a CI gate — PLAN §11/F10:
shared runners are noisy, and throughput thresholds in tests rot). Machine for
the numbers below: dev workstation, Linux 7.2.8 x86_64 (Fedora), Node 24.18.0,
g++ 15.3, Release build, 18 cores, otherwise idle. Re-run with `npm run bench`.

## Results (2026-10-06, membridge 0.0.0)

| Bench | Value |
|-------|-------|
| contention — same-process `Atomics.add` (5 M ops on a membridge SAB) | ~110–120 M ops/s |
| contention — cross-process futex handoff (ping/pong round trip, shared futex) | ~0.002 ms/round-trip (~500–580k/s) |
| mutex — uncontended `lock`+`unlock` (100k iters, same thread) | ~0.18 µs/op |
| mutex — holder SIGKILLed → steal (SIGKILL → acquired) | < 250 ms (one detection slice; typically ~0 ms when the kill is observed immediately) |
| ring — 64 B messages (copying `write`/`read`) | ~2.0–2.4 M msgs/s (~150 MB/s) |
| ring — 4 KiB messages | ~330–360k msgs/s (~1.3–1.5 GB/s) |
| ring — 64 KiB messages | ~28–32k msgs/s (~1.8–2.1 GB/s) |

## Reading them

- The `Atomics.add` line is the floor: pure user-space atomics over the shared
  page, no syscalls. Everything cross-process adds futex syscalls.
- The handoff line is two processes ping-ponging one word over shared futexes —
  the primitive §7/§8 blocking is built on. ~2 µs per round trip is the
  cross-process synchronization cost.
- Ring numbers use the **copying** convenience API (`write`/`read`) — one copy
  in and one copy out. The zero-copy `reserve`/`commit` + `peek`/`release` path
  avoids the payload copies entirely; these figures are the conservative bound.
- Mutex recovery is state-based, not timing-based: after a SIGKILL (and reap)
  the contender's next 250 ms wait-slice liveness check observes a dead owner
  and steals immediately.
