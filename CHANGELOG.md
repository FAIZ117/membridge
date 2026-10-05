# Changelog

All notable changes to membridge are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versioning follows SemVer.

## [Unreleased]

Nothing published yet. Planned scope (see [PLAN.md](./PLAN.md)):

- Hardened core segments — header page, size policies, typed errors (M2)
- Cross-process wait/notify via `membridge/sync` (M3)
- Crash-safe Mutex (M4)
- Zero-copy RingBuffer (M5)
- Ops utilities: `capacity`, `stat`, `list`, `reap`, `unlinkWhenUnused` (M6)

### Release process

Publishing follows the same manual pattern as `expr-eval-nextgen`: CI assembles
the publish artifact (with all prebuilds), the owner runs `npm publish` — see
[docs/release.md](./docs/release.md).
