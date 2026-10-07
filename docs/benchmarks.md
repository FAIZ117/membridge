# Benchmarks

Informational numbers from `npm run bench` (never a CI gate — PLAN §11/F10:
shared runners are noisy, and throughput thresholds in tests rot). Machine for
the numbers below: dev workstation, Linux 7.2.9 x86_64 (Fedora 43), Node 24.18.0,
g++ 15.3, Release build, 18 cores. Re-run with `npm run bench`. Ranges are
min–max over the runs of one session; the box was not fully idle, so treat
spreads of ±25% on the ring rows as noise.

## Results (2026-10-07, after the round-3 fixes)

| Bench | Value |
|-------|-------|
| contention — same-process `Atomics.add` (5 M ops on a membridge SAB) | 94–98 M ops/s this session (~119 M on an idle box, 2026-10-06) |
| contention — cross-process futex handoff (both sides parked, round trip) | ~0.006 ms/round-trip (~165–180k/s) |
| mutex — uncontended `lock`+`unlock` (100k iters, same thread) | **0.07–0.10 µs/op** in the bench; 49–51 ns in a 2M-iteration loop (same-session `248f731` baseline: 48–59 ns — no regression) |
| mutex — 3-way contended handoff | not produced by `npm run bench`; the round-3 performance pass measured 0 waits ≥ 250 ms in 31 tight-loop runs (5 µs critical section, 0 think time, 3/6/8 processes) |
| mutex — holder SIGKILLed → steal (SIGKILL → acquired) | ~0 ms (zombie-aware liveness) |
| ring — 64 B messages (copying `write`/`read`) | **2.2–3.5 M msgs/s** (same-session `248f731` baseline 2.3–3.3 M) |
| ring — 4 KiB messages | 279–388k msgs/s (1.1–1.6 GB/s) |
| ring — 64 KiB messages | 25–30k msgs/s (1.6–2.0 GB/s) |
| ring — async (`reserveAsync`/`peekAsync`) | sub-millisecond wakes: worst `reserveAsync` 0.4–0.9 ms over 8 × 20k messages into a full 4 KiB ring; `test/round3.test.ts` fails any async round trip ≥ 150 ms |

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
