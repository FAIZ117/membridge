# Changelog

All notable changes to membridge are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versioning follows SemVer.

## [Unreleased]

Nothing published yet. Implemented on `main` (M1–M8, see [PLAN.md](./PLAN.md)
§14 and the milestone commits):

- Plain-V8 native addon (node.h, no N-API): SharedArrayBuffer windows over OS
  shared memory with a crash-safe mapping registry (M1/M2)
- Hardened core segments — header page with init protocol and attach table,
  size policies (`exact`/`at-least`/`grow`), posix_fallocate reserve,
  typed `MembridgeError`s, opt-in in-process fallback (M2)
- Cross-process wait/notify via `membridge/sync` — shared futexes on Linux,
  `futex_waitv` multiplexer thread, `os_sync`/semaphore paths for
  macOS/Windows (M3)
- Crash-safe Mutex — single-word token CAS, dead-holder steal with
  `ownerDied` reporting, slot reclamation, `lockAsync` with AbortSignal (M4)
- Zero-copy SPSC RingBuffer — two-phase `reserve`/`commit` + `peek`/`release`,
  SKIP framing, role claims, at-least-once consumer-crash semantics (M5)
- Ops utilities — `capacity`, `stat`, `list`, `reap`, `unlinkWhenUnused` (M6)
- CI matrix (3 OS × Node 22/24/26 + ASAN/UBSan + tmpfs job), per-ABI
  prebuild pipeline, ESM wrappers, API docs + compatibility matrix,
  benchmarks (M7)

### Pre-release review fix round (2026-10-06)

Full report: [docs/review/2026-10-06T1909-pre-release-review.md]. Six fix
commits closing the release blockers:

- **Security:** header geometry is bounded by the real mapping on every join
  (hostile/stale headers can no longer size a SAB past mapped memory — the
  cross-process OOB class), attach-row indexing is mapping-bounded, registry
  reuse validates object identity (dev/ino) so unlink+recreate can no longer
  split-brain a process, and reads of short objects expose no uninitialized
  memory.
- **Correctness:** ring consumer survives the 2^31 counter wrap; mutex slots
  are freed when workers exit (no more 64-slot exhaustion); sequential
  workers' `waitAsync` promises always settle (stale hub reuse); zombie
  holders are stolen from; the creator publishes `initState=1` before sizing
  so joiners never take over a live creator; grow never shrinks and always
  covers its window; macOS/Windows paths compile and match the documented
  mechanisms.
- **Performance:** per-lock native bookkeeping removed (mutex lock/unlock
  0.18 → 0.07 µs), mutex wake protocol preserves HAS_WAITERS (no more
  slice-bound waits on a free lock), ring parked flags (64 B messages
  2.3M → 3.2M msgs/s), wait slices clamp to the caller's remaining timeout,
  macOS async waits use the timed `os_sync` variant.
- **API:** `reserveAsync`/`peekAsync`, ring join validates
  capacity/maxMessage, `waitAsync` resolves `'not-equal'` per the §6
  contract, `stat()` liveness uses the §7.1 native check.

### Release process

Publishing follows the same manual pattern as `expr-eval-nextgen`: CI assembles
the publish artifact (with all prebuilds), the owner runs `npm publish` — see
[docs/release.md](./docs/release.md).
