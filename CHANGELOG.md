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

### Round-3 fix round (2026-10-07)

Re-verification: [docs/review/2026-10-07T1236-round3-reverification.md]. Fix
handoff and verification guide: [docs/review/2026-10-07T1349-round3-fixes.md].

- **Mutual exclusion restored** (C1–C4): slot and role claims take an
  exclusive state-word right (`pid << 2 | 2`), are serialized per process, and
  a reclaim re-validates the gen it judged; each JS instance holds its own
  claim reference, so closing or collecting one `Mutex` never releases a lock a
  sibling instance holds (ADR 0007).
- **Memory safety**: the attach-row unwind after a grow resolves the header
  from the re-mapped handle (C5); Windows `stat()` bounds its row loop by the
  mapped view (C8); native views are checked for offset and length, and the
  claim pin comes from the view's own buffer (C18).
- **Ring**: async waits snapshot the counter before their attempt — no lost
  wakeups (C6); `close()` for both roles with `E_CLOSED` (C10); a new role
  holder clears a stale parked flag (C13); `RingProducer.open` returns the
  thread's live producer (C24); timeouts validated (C14); `reserveAsync` no
  longer throws per failed attempt (C19). An exchange-based wake (C20) was
  tried and reverted — it lost wakes (ADR 0006).
- **Init protocol**: an epoch in the init word closes the handback ABA (C16);
  every join pass honors `initTimeoutMs` (C11); a takeover keeps a valid prior
  header's kind and geometry (C12); failed takeovers release their row (C17);
  size-less joins of uninitialized objects no longer truncate them and use one
  timeout budget (C23).
- **Identity / liveness**: failed self-identity reads are retried, not cached
  (C9/R19); macOS liveness matches Linux (zombies, unreadable = unknown);
  Windows `stat()`/`reap()` use the native liveness check (F34).
- **Registry**: the attach row is handed to a surviving mapping (C15); a kind
  mismatch no longer evicts the valid entry; Linux identity check is one
  `lstat` (C22).
- **Platforms**: macOS uses timed `os_sync` waits with the correct wake flags
  and refuses `grow` with `E_GROW_UNSUPPORTED` (C7) — CI-unverified;
  `posix_fallocate` errors map precisely (`E_NO_SPACE` only for
  ENOSPC/EFBIG, EINTR retried) (C21).
- **Post-verification hardening**: an own slot that would mint token 0 is
  re-published before use; a role claim that meets a live participant
  mid-claim waits up to 100 ms for it to settle instead of failing fast; the
  registry's mapping list is pruned on every insert.
- **API**: new error code `E_CLOSED`; mutex/ring `timeoutMs` errors are
  `E_NAME_INVALID` (matching `membridge/sync` and PLAN §7.3).

### Round-4 fix round (2026-10-07)

Review: [docs/review/2026-10-07T1710-round4-review.md]. Fixes and verification
guide: [docs/review/2026-10-07T1753-round4-fixes.md].

- **Mutex**: `lockAsync` pending at `close()` rejects `E_CLOSED` instead of
  acquiring with a dropped claim (E4-1); `tryLock()` now returns
  `{ ownerDied } | null` and the death-release publishes the flag before
  freeing the word (E4-3).
- **Ring**: a `reserveAsync` that wakes while another reservation on the same
  producer is uncommitted rejects `E_RING_STATE` instead of handing out the
  same region (E4-2).
- **Init protocol**: the creator reserves (`posix_fallocate`) only after it
  holds the init baton, so a joiner can no longer take over a live creator
  mid-reserve; a takeover never shrinks below the object a dead creator sized
  (E4-4).
- **Async waits**: one truncated segment no longer freezes every async wait in
  the process; the fallback waiter thread no longer spins on EFAULT (S4-1).
- **`/dev/shm` hygiene**: FIFOs and symlinks there can neither hang nor abort
  `list()`/`stat()`/`reap()`; failed opens no longer leak fds (S4-2/S4-5/S4-7).
- **Containers**: the in-flight slot-claim marker carries a pid-namespace tag;
  claims are never recovered across namespaces (S4-3/E4-6/E4-7).
- **Scalability**: claim cost no longer grows with mapped segments (P4-1);
  registry teardown and inserts are per object, not O(mappings) (P4-2/P4-3);
  the role-claim settle wait no longer blocks other claims (P4-4).
- **Registry**: `unlinkWhenUnused` is honoured on reuse and non-owner
  mappings (E4-8); a raw open no longer displaces a typed entry (E4-10).
- **Packaging**: the fault-injection hook is inert unless
  `MEMBRIDGE_TEST_HOOKS=1`; compiled tests are no longer shipped (S4-14).

### Release process

Publishing follows the same manual pattern as `expr-eval-nextgen`: CI assembles
the publish artifact (with all prebuilds), the owner runs `npm publish` — see
[docs/release.md](./docs/release.md).
