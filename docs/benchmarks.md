# Benchmarks

Informational numbers from `npm run bench` (never a CI gate — PLAN §11/F10:
shared runners are noisy, and throughput thresholds in tests rot). Machine for
the numbers below: dev workstation, Linux 7.2.9 x86_64 (Fedora 43), Node 24.18.0,
g++ 15.3, Release build, Intel Core Ultra 5 125H — a hybrid CPU with 14 cores
(4 P + 8 E + 2 LP-E) and 18 threads, so single-thread numbers are bimodal
(about 48 ns on a P-core vs 66–85 ns on an E-core for lock+unlock; pin with
`taskset` for stable figures). Re-run with `npm run bench`. Ranges are
min–max over the runs of one session; the box was not fully idle, so treat
spreads of ±25% on the ring rows as noise. The ring rows use a producer in a
worker thread; cross-process figures are similar (64 B copying 1.95–2.51 M
msgs/s, round-4 performance pass).

## Results (2026-10-10, v0.2.0)

| Bench | Value |
|-------|-------|
| contention — same-process `Atomics.add` (5 M ops on a shm-bridge SAB) | ~106 M ops/s |
| contention — cross-process futex handoff (both sides parked, round trip) | ~0.006 ms/round-trip (~170k/s) |
| mutex — uncontended `lock`+`unlock` (100k iters, same thread) | **~0.08 µs/op** |
| mutex — holder SIGKILLed → steal (SIGKILL → acquired) | ~3 ms (zombie-aware liveness) |
| ring — 64 B messages (copying `write`/`read`, worker-thread producer) | **~3.1 M msgs/s** (~198 MB/s) |
| ring — 4 KiB messages | ~326k msgs/s (~1.3 GB/s) |
| ring — 64 KiB messages | ~29.5k msgs/s (~1.9 GB/s) |
| ring — async (`reserveAsync`/`peekAsync`) | wakes typically < 1 ms; `test/round3.test.ts` fails any async round trip ≥ 150 ms |

Historical ranges from the 0.1.0-era runs (2026-10-06/07, same machine):
Atomics.add 94–119 M ops/s; handoff ~155–180k/s round trips; uncontended
lock 49–100 ns (bimodal across P/E cores); the round-3 performance pass
measured 0 mutex waits ≥ 250 ms in 31 tight-loop runs (5 µs critical
section, 0 think time, 3/6/8 processes); ring 64 B 2.2–4.4 M msgs/s;
4 KiB 279–388k; 64 KiB 24–38k; async worst `reserveAsync` 0.44–3.78 ms over
8 × 20k messages into a full 4 KiB ring. The v0.2.0 numbers sit inside all
of those ranges — no regression from the cross-platform rounds.

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
- Mutex recovery is state-based, not timing-based: the contender probes the
  holder's liveness on its first contended pass and again after any wait slice
  that timed out, so a SIGKILLed holder (zombie or reaped) is stolen from on
  the first probe — ~0 ms in the bench.
