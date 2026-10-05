# membridge — Implementation Plan

> **Status: planning — awaiting review.** Nothing is implemented yet; this repo is a skeleton.
> Name `membridge` verified free on npm (registry 404, checked 2026-10-05).

A new npm package with complete ownership that keeps the good parts of
[`shmbuf`](https://www.npmjs.com/package/shmbuf) (MIT, by kedemd) — the hardened
starting point — while fixing its bugs, optimizing, and adding features.
membridge supersedes shmbuf for our use.

**Scope decisions (locked):** TypeScript · crash-safe Mutex · zero-copy RingBuffer ·
ops utilities.

## 1. Location & ownership

- Standalone git repo at `/home/faiz/work/membridge/` (this repo).
- MIT LICENSE, author "Faiz" (placeholder — edit before publishing).
- README credits shmbuf (MIT) as the hardened starting point: honest attribution,
  full ownership of the new code.
- Publishing to npm is done by the owner, not by the agent. `package.json` starts
  `"private": true` to block accidental publishes.

## 2. Verified facts driving the design

- **No N-API SAB-creation API exists** (grepped Node 18–24 headers): wrapping
  mmap'd memory as a `SharedArrayBuffer` requires the direct-V8 backing-store
  technique (`v8::SharedArrayBuffer::NewBackingStore` + GC finalizer). shmbuf's
  core trick is the right one — verified working (all tests pass, and the returned
  SAB transfers to `worker_threads` sharing the same physical memory, on Node 24).
- **Real limits:** `/dev/shm` capacity (7.6 GB on the dev machine; default mount =
  50% of RAM; tmpfs charges pages lazily on touch, and a fill-up mid-life
  manifests as SIGBUS, not a JS error). shmbuf's 64 MB cap is an arbitrary addon
  guardrail, not an OS limit. On Windows the parallel is pagefile-backed sections
  (ceiling ≈ RAM + pagefile).
- **Toolchain present:** gcc/g++/make/python3, node-addon-api 8.9.2, node-gyp
  header caches for Node 18/20/22/24.

## 3. Kept from shmbuf (already good)

- `shm_open` + `ftruncate` + `mmap` → `v8::SharedArrayBuffer::NewBackingStore`
  with GC finalizer; POSIX EEXIST-rejoin path; Windows `CreateFileMapping` path.
- API shape `open(name, size) / close(name) / unlink(name)`.
- Zero-runtime-dependency design; graceful fallback mode with warning.
- `get-include-dir.js` + `binding.gyp` skeleton.
- Test structure: fork-based cross-process test, 8-process high-contention perf
  test (~11M ops/s combined baseline as a regression gate).

## 4. Bugs & design flaws fixed

1. **SIGBUS class (the killer).** On join, `fstat` the existing segment and
   compare with the requested size. Default policy **strict**: throw
   `E_SIZE_MISMATCH` naming both sizes. Opt-in `grow: true` extends the segment
   via `ftruncate` (grow-only). This also closes the create/join race where a
   joiner maps the segment before the creator's `ftruncate` lands.
2. **Lifecycle.** Refcounted registry per name: same-name re-open returns the
   *same* SAB (optimization — no re-mmap); the mapping is released when the
   refcount hits zero AND all SABs have been GC'd. `close()`/`unlink()` become
   meaningful and their semantics documented; shmbuf's vestigial `g_regions`
   map is dropped.
3. **Typed errors.** `MembridgeError` with machine-readable codes:
   `E_SIZE_MISMATCH`, `E_EXISTS`, `E_NOT_FOUND`, `E_NAME_INVALID`,
   `E_SIZE_INVALID`, `E_NOT_OWNER`.
4. **Validation.** Name must start with `/` and be ≤ 250 chars; size between
   1 and the configured max.
5. **Configurable size cap.** Default 256 MiB (raised from shmbuf's hardcoded
   64 MB), overridable via env `MEMBRIDGE_MAX_SEGMENT_BYTES`. Docs cover tmpfs
   lazy charging and the SIGBUS-on-fill hazard.
6. **Honest documentation.** `close()` does not synchronously unmap (GC-deferred
   by design — unmapping under a live SAB would be use-after-free); processes
   opening the same name must agree on size or use the explicit policies;
   segments persist in `/dev/shm` after a crash until `unlink()`/cleanup.

## 5. New features (v1.0.0)

### Crash-safe Mutex
Own 32-byte segment; layout `[0]=state [1]=ownerPid [2]=heartbeat [3]=ownerGen`.
- Lock: CAS 0→1; on contention, `Atomics.wait(state, 1, 50ms)` loop.
- Dead-owner recovery: heartbeat timeout + `process.kill(pid, 0)` ESRCH check →
  steal the lock by generation CAS. A crashed process cannot deadlock everyone.
- `unlock` verifies ownership, then `Atomics.notify`; `withLock(fn)` wrapper.
- Sync/blocking semantics documented (Atomics.wait blocks the event loop).
- Step-0 spike validates cross-process futex wake on Linux; a spin fallback
  flag exists if wake/notify ever fails to fire cross-process.

### Zero-copy RingBuffer (SPSC)
Segment = magic + version + capacity + head/tail u32 (Lamport: one empty slot)
+ data region. `write(view) / read()`. Cross-process tested. Capacity up to ~2 GB.

### Ops utilities
- `capacity()` — `fs.statfs` on `/dev/shm` (free/total).
- `stat(name)` — per-segment info.
- `list()` — readdir `/dev/shm` filtered by our prefix.
- `cleanupOnExit()` / `clearExitCleanup()` — unlink still-registered segments on
  process exit (protects against crash-leaked segments).

## 6. Layout

```
membridge/
  package.json  tsconfig.json  LICENSE  README.md  .gitignore  PLAN.md
  .github/workflows/ci.yml
  src/    index.ts core.ts mutex.ts ringbuffer.ts ops.ts errors.ts fallback.ts
  cc/     segment.cc segment.h binding.gyp get-include-dir.js
  test/   *.test.ts   (node:test runner — zero test dependencies)
  bench/  bench.js    (contention + mutex + ringbuffer benches)
```

- CJS + ESM builds via tsc (`exports` map); `engines >= 18`.
- Install strategy: per-ABI prebuilds via prebuildify + node-gyp-build resolver
  (the addon is V8-ABI-bound, not NAPI-generic — prebuilds must be per-ABI),
  with node-gyp source-build fallback.

## 7. Tests (node:test; ported + new)

Ported from shmbuf: the 20 behaviors, fork cross-process handshake, 8-process
contention/perf gate (no lost updates; ≥ 5M ops/s combined floor).

New: strict/grow size policies, join-race (N processes racing the same name),
create modes, error codes, refcount/close semantics, cleanupOnExit, Mutex
(same-process, cross-process, dead-owner steal), RingBuffer cross-process,
fallback mode, `worker_threads` sharing. ASAN/valgrind job in CI; matrix
Node 18/20/22/24 × linux (+ win/mac smoke).

## 8. Milestones (agentic rounds)

| # | Milestone | Size |
|---|-----------|------|
| 1 | Spike: cross-process `Atomics.wait`/`notify` + v8 SAB behavior on Node 24 | short round |
| 2 | Hardened native core + TS layer + typed errors | 1–2 rounds |
| 3 | Mutex (1 round) → RingBuffer (1 round) → ops utils + fallback polish (short) | ~2 rounds |
| 4 | Tests, CI, prebuild pipeline, docs | 1–2 rounds |
| 5 | Full verification: multi-process suites on Node 18/22/24, `npm pack` dry-run | short round |

Estimated total: **~6–9 agent rounds**.

## 9. Decisions you may want to override

- Author name in `package.json` / `LICENSE` is a placeholder ("Faiz").
- Default max segment size 256 MiB (vs shmbuf's 64 MB) — adjustable.
- Size-mismatch default policy is **strict** (error) rather than auto-grow —
  safer for production; flip to grow-first if you prefer shmbuf's implicit behavior.
- GitHub repo URL / CI badges are left out until you create the remote.
