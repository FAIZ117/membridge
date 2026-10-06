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

### Release process

Publishing follows the same manual pattern as `expr-eval-nextgen`: CI assembles
the publish artifact (with all prebuilds), the owner runs `npm publish` — see
[docs/release.md](./docs/release.md).
