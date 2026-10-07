# Round-3 re-verification — 2026-10-07 12:36 (+05:30)

| | |
|---|---|
| Commit verified | `248f731` (10 commits on top of `0f01571`), clean working tree |
| Previous rounds | [2026-10-06T1909-pre-release-review.md](./2026-10-06T1909-pre-release-review.md) · [2026-10-06T2254-fix-round-reverification.md](./2026-10-06T2254-fix-round-reverification.md) (round 2 — IDs R1–R24 refer to it) |
| Fix commits | `23fc445` G (geometry read once, mapping re-widened) · `da6a2bc` H (mutex pin from BackingStore, gen-CAS reclaim, always-OR wake) · `77e7412` I (async ring parked flags) · `c39bd11` (Windows VirtualQuery join, macOS os_sync symbols) · `7497650` L (R18/R19/R21, F20, docs) · `3be15d3` (benchmarks.md) · `b6c8b65` M (atomic init baton) · `a329068` N (F17 drain, F33 kind) · `86faff1` O (F23 slot machine, consumer `close()`) · `248f731` P (F36 docs) |
| Platform of evidence | Linux 7.2.9 (Fedora 43), x86_64, 18 cores, Node 24.18 |
| Build | `npx node-gyp rebuild` clean; 2 warnings (upstream `node.h` cast; unused `HeaderDataBytes(Header*)` in `cc/addon.cc` — dead because all four old call sites moved to the validated snapshot) |
| Suite | `npm test`: 93 tests, 92 pass, 1 env-gated skip |
| Mode | Read-only. Three re-verification passes plus independent repros. Regression attribution used scratch builds of `0f01571` and `da6a2bc`. All segments used `/membridge-test-rv3*-` names; `/dev/shm` clean afterwards. |

**Coverage limits.** The security pass was source-only, and a live
header-flipping race check was not run; security conclusions rest on source
tracing plus the repros in §2. macOS and Windows statements are from source
reading only.

---

## 1. Summary

**Most round-2 items now reproduce as fixed.** The geometry rework holds:
cross-process prefix opens work, concurrent create/join and grow produce no
spurious errors or short windows, the race soak is clean, the init baton
prevents multiple initializers, and creator failures no longer strand names.
Mutex teardown no longer crashes, claimed mutexes no longer leak fds or
mappings, the mutex lost-wakeup residual is gone (0 slice-bound waits in
~650k tight-loop acquisitions and 31 benchmark runs), and async ring waits
wake in milliseconds instead of 230 ms.

**Not publishable.** Commits H and O, which reworked mutex claiming and
lifetime, broke the mutex's core guarantee — **two holders can be inside the
lock at the same time** — through three independent paths, each reproduced
end to end through the public API and attributed by building the earlier
commits. Commit G's new row guard writes into unmapped memory after a failed
grow. The async ring re-check is still racy, and macOS waits are now untimed.

### Publish blockers

1. **Mutual exclusion** (C1–C4): one native claim entry per *instance* (or
   one JS instance per thread+segment); per-thread claim marker; CAS gen
   from the value read before the liveness verdict; same for ring roles.
2. **Row guard after remap** (C5): resolve the header from the handle at
   unwind time.
3. **Async ring re-check** (C6): snapshot the counter before the
   non-blocking attempt and compare against it (two-line patch measured to
   remove every stall).
4. **R19** (C9): do not record a failed identity read as cached.
5. **`close()` must disable the instance** (C10).
6. **Platforms** (C7, C8): fix macOS timed waits and the Windows
   `ReadHeader` bound — or mark both unsupported in `docs/compat.md` and keep
   them out of release claims until CI passes.
7. **Tests for the above**: none of C1–C5, C10 or R19 is visible to the
   93-test suite (§7).

### Counts

| Pass | Carried items checked | Fixed | Partial | Not fixed | Documented only | Regressed | New |
|---|---|---|---|---|---|---|---|
| Correctness (F + R items) | 44 | 26 | 11 | 1 | 3 | 3 | 16 |
| Performance (P1–P11 + R-items) | 16 | 11 | 2 | 2 | 1 | 0 | 7 |
| Security (R4/R5/R10/R7/R8/R23, F1–F8) | 14 | 10 | 2 | 0 | 1 | 1 | 5 |

Merged across passes: **25 distinct new findings (C1–C25)** — 2 P0, 6 P1
(two of them platform-specific), 4 P2, 13 P3.

---

## 2. Independent checks

| Check | Result |
|---|---|
| R1 — fresh process opens an existing 8192-byte segment | at-least 4096 → OK (4096); grow 4096 → OK (4096); exact 4096 → `E_SIZE_MISMATCH`; exact 8192 → OK; at-least 16384 → `E_SIZE_MISMATCH`. **Fixed.** |
| F2 — file shorter than its header (exact / at-least / grow / `Mutex.open` / size-less) | All `E_INCOMPATIBLE`, no SIGBUS. **Holds.** |
| R6 — size-less join of a 0-byte object | `E_INIT_TIMEOUT` after 5,279 ms (no ready 0-byte segment published); a following `open(name, 65536)` succeeds. **Fixed** (see C23 for the latency). |
| F5 / R2 / R3 — 15 s soak: 1 grower + 2 size-less joiners vs `/proc/self/maps`; create/unlink/recreate + 3 joiners writing first and last byte | 256,550 joins, **0 oversized SABs, 0 errors** (round 2: ~0.1% spurious `E_INCOMPATIBLE`); grower **0 short SABs**; 9,672 touching joins, **0 SIGBUS**, only `E_NOT_FOUND`. |
| C1 (T1) — two `Mutex` instances on one thread; B holds; A is closed | Another process acquires with `ownerDied: true` while B still holds; B's `unlock()` → `E_NOT_OWNER`. **Mutual exclusion broken — confirmed.** |
| C5 (U1/T5) — row guard | `~RowGuard` uses `guard.h.base` captured before `GrowSegment`/`EnsureMappingCovers` (`cc/addon.cc:406-413`, `:421`, `:438`); `RemapSegment` munmaps the old base (`cc/segment.cc:369`); errors unwind as C++ exceptions (`throw NativeError{}`, `-fexceptions`). **Confirmed in source.** |
| C6 (Q1/T10) — async ring re-check | `const tailNow = this.tail(); if (tailNow !== this.tail()) continue;` (`src/ringbuffer.ts:313-314`; same in `peekAsync` `:484-485`) compares two back-to-back reads. **Confirmed in source.** |
| C7 (Q3/T6) — macOS waits | `os_sync_wait_on_address` (untimed) in `SyncWait` (`cc/wait.cc:547`) and the wait thread (`:342`). **Confirmed in source.** |
| C13 (R15a) — parked-flag reset on role takeover | No reset in `cc/` or in the ring open paths. **Not landed — confirmed.** |

---

## 3. Status of carried-over items

### 3.1 Correctness — F-items

| ID | Round 2 | Now | Evidence |
|---|---|---|---|
| F4 | PARTIAL P0 | PARTIAL | macOS `CheckLiveness` has no zombie check (`cc/liveness.cc:193-204`) |
| F5 | PARTIAL P1 | FIXED (verified) | 60 crafted runs: 0 runs with >1 initializer |
| F8 | PARTIAL P1 | FIXED (verified) | concurrent growers 8×150, 4×300: 0 short SABs |
| F10 | PARTIAL P1 | FIXED (verified) | see R13 |
| F12 | PARTIAL P1 | FIXED (verified) | mutex and ring producer at gen 0x7FFF → token 1 |
| F13 | NOT FIXED P1 | **REGRESSED** | became C3 |
| F14 | PARTIAL P1 | FIXED (verified) | unlink → worker first lock → terminate: main acquires with ownerDied, 10/10 |
| F15 | PARTIAL P1 | DOCUMENTED ONLY | `docs/compat.md:71` |
| F17 | PARTIAL P1 | FIXED (reading) | `cc/wait.cc:764-778` |
| F18 | PARTIAL P1 | FIXED (reading) | relies on libuv's async-close spin |
| F19 | NOT FIXED P1 | PARTIAL | compiles against real SDK symbols by reading; waits untimed (C7) |
| F20 | NOT FIXED P2 | PARTIAL | premature release fixed; leaks a row instead (C15) |
| F23 | PARTIAL P2 | **REGRESSED** | crashed-publish recovery works; the two-state machine causes C2/C4; `close()` does not disable (C10) |
| F25 | PARTIAL P2 | PARTIAL | role claims refuse `startTime -1`; failure still cached (C9) |
| F27 | REGRESSED P2 | FIXED (reading) | VirtualQuery join (`cc/segment.cc:132-138`); not run in CI |
| F30 | PARTIAL P2 | FIXED (verified) | see R14 |
| F31 | NOT FIXED P3 | DOCUMENTED ONLY | `docs/compat.md:74` |
| F33 | PARTIAL P3 | FIXED (verified) | same-process kind mismatch → `E_INCOMPATIBLE`; evicts the valid entry (C15) |
| F34 | PARTIAL P3 | PARTIAL | Windows liveness hard-coded `'unknown'` (`src/ops.ts:103-104`) |
| F35 | PARTIAL P3 | PARTIAL | ring NaN timeout waits forever (C14) |
| F36 | PARTIAL P3 | DOCUMENTED ONLY | PLAN §5.6 + test |
| F37 | PARTIAL P3 | FIXED (reading) | prune `cc/wait.cc:681-687`; `docs/compat.md:80` |

### 3.2 Round-2 R-items

| ID | Round 2 | Now | Evidence |
|---|---|---|---|
| R1 | P1 | FIXED (verified ×2) | §2 |
| R2 | P1 | FIXED (verified ×2) | 0 spurious errors: 8×150 (1 MiB), 2×500, 8×130 (64 MiB), `Mutex.open` 8×150, 2×500; soak §2 |
| R3 | P1 | FIXED (verified ×2) | 2,400 concurrent grows, 0 short; soak §2 |
| R4 | P0 race | FIXED (reading, two passes) | every row scan uses a count bounded by the mapping (`cc/header.cc:110-122,217,295,333,372,427`; Detach `cc/registry.cc:72-74`) |
| R5 | P0 race | FIXED (reading) | `dataBytes` read once (`cc/header.cc:256,180`), window checked vs mapping (`cc/addon.cc:447-455`); `DataAddrOf` checks length but not byte offset (internal callers only — C18) |
| R6 | P2 | PARTIAL | 0-byte case fixed (§2); takeover after a crashed **grow** still rewrites kind and geometry (C12) |
| R7 | P2 | FIXED (verified) | 60 runs, 0 multi-initializer; residuals C16 |
| R8 | P2 | FIXED (reading) | inherent fstat/ftruncate TOCTOU needs a 250 ms creator stall |
| R9 | P1 | FIXED (verified) | `ulimit -f 64` creator and takeover joiner: name removed or recoverable |
| R10 | P2 | FIXED (reading) | floor from `st_size`, capped (`cc/header.cc:458-467`) |
| R11 | P1 | FIXED (verified) | 40 workers claiming after unlink, GC'd: no SIGSEGV |
| R12 | P1 | FIXED (verified ×2) | 5,000 mutexes: mappings/fds back to 0/21 after GC |
| R13 | P2 | FIXED (verified ×2) | 0 waits ≥ 250 ms in ~650k acquisitions and 31 benchmark runs |
| R14 | P1 | PARTIAL | single wakes 0.04–0.50 ms (was 230 ms); racy re-check remains (C6) |
| R15 | P3 | PARTIAL | R15a reset never landed (C13); R15b second consumer refused, but C4/C10 |
| R16 | P1 | FIXED (reading) | not run in CI |
| R17 | P1 | PARTIAL | real symbols; untimed (C7) |
| R18 | P3 | FIXED (measured) | 1.000 liveness check per contended lock; 2.4–2.7 µs/check |
| R19 | P2 | **NOT FIXED (verified)** | C9 |
| R20 | P2 | PARTIAL | mutex rejects NaN/Infinity/≤0; ring NaN waits forever (C14) |
| R21 | P3 | FIXED (reading) | `cc/wait.cc:464-472,507-514` |
| R22 | P3 | PARTIAL | grace floor gone (racing join p50 0.06–0.25 ms, 0 errors); reuse still 2.55–2.88 µs (C22) |
| R23 | P3 | **REGRESSED** | the new row guard is C5; failed-takeover row leak remains (C17) |
| R24 | P3 | PARTIAL | new drift (C25) |

### 3.3 Performance — P-items

"Before" from a same-session scratch build of `0f01571`.

| ID | Now | Before | After |
|---|---|---|---|
| P1 | FIXED | 0.067–0.087 µs | 0.047–0.055 µs; 100/500 mappings 0.047–0.051; 1/2/4/8 threads 0.048–0.086 |
| P2 | **FIXED** | 5 µs CS / 0 think, 3/6/8 procs: 29–30 / 70–78 / 111–112 waits ≥ 250 ms, max 502–754 ms | 0 such waits in 31 runs (incl. `lockAsync`); 159–163k acq/s, max 47–121 ms. Round-2 config: 41.4–42.3k acq/s, max ≤ 13.8 ms |
| P3 | FIXED | zero-copy 8 B 3.3–4.3M msgs/s | 3.6–3.9M; 4 KiB 2.4–2.6M; 64 KiB 1.8–2.0M; producer-side wakes 0–0.06/msg (full ring: C20) |
| P4 | FIXED | — | 1 → 1.02–1.68 ms; 10 → 10.03–10.82; 50 → 50.07–50.75 (all six APIs) |
| P5 | NOT FIXED | — | untimed macOS parks + 50 µs sleep per wake (C7) |
| P6 | DOCUMENTED ONLY | 7.1–47.3 µs | 7.3–45.0 µs at 0/32/64/126 parked (unchanged) |
| P7 | FIXED | 3.1–4.05 µs/check, 1.01–10.3 checks per acquisition | 2.44–2.74 µs; exactly 1.000 per contended `lock()` |
| P8 | FIXED | 6.6–9.2 µs | 6.5–10.7 µs flat 0–62 slots; first `lock()` +3.5 µs once per Mutex |
| P9 | FIXED | stat 8.2–9.2 µs | stat 7.2–9.1 µs; `reap({dryRun})` over 500 segments 4.9–7.5 ms |
| P10 | FIXED | — | wakes on ready, error restore and handback |
| P11 | FIXED | — | handoff 6–7 µs round trip |

No regression in commit G open paths (create 12.6–15 µs, now 1 ftruncate +
1 mmap; cold join 6.7–8.3 µs; only prefix joins re-map), commit H after a
contention burst (0 extra wakes, 0.052 µs/op), commits I/O on the sync ring
path, or worker lifecycle (p50 24.5–28.5 ms, 0 leaked slots after 30 exits).

### 3.4 Security

| Item | Now | Evidence |
|---|---|---|
| R4, R5, R10 | FIXED (R5 mostly) | see §3.2 |
| R7/R8 | PARTIAL | baton bounded (`cc/header.cc:369-377`), `ErrRestorer` restores on failure; retry paths skip the deadline (C11) |
| R23 | REGRESSED | C5 |
| F1–F6 | HOLD | hostile `dataBytes` rejected (`cc/header.cc:274-290`); short files and soak verified (§2); identity check (`cc/registry.cc:98-114`) |
| F7 (macOS) | PARTIAL | `proc_pidinfo` failure → alive (fail-safe); no zombie detection; undocumented for macOS |
| F8 | DOCUMENTED ONLY | `docs/compat.md:24` |

Shared-memory reads after validation (security pass, confirmed by the
correctness pass): `initState` (`cc/header.cc:248`, per-iteration local,
packed slot checked `< n`), `initializerSlot` (`:370`, checked `>= 0 && < n`),
`headerBytes` (`:252` validated; `:371` bounded by mapping),
magic/version/flags (`:253-255`, compare only), `dataBytes` (`:256`, `:180`,
one snapshot each; TryReuse `cc/addon.cc:263-287` re-reads but bounds by
mapping), attach rows (bounded by `nslots`), mutex token slot
(`cc/mutex.cc:201-203`), ring control words (JS, masked) — all safe on
POSIX. **Exception: Windows `ReadHeader`** (C8).

Cleared list re-confirmed by source: symlink (`O_NOFOLLOW`), malformed token
(`cc/mutex.cc:203`), bounded claim loops (`cc/mutex.cc:123,145,180,474`),
ring arithmetic masked and unsigned, POSIX `ReadHeader` bounded by bytes read
(`cc/segment.cc:446-449`), forged rows cannot make `reap()` unlink a live
segment, `WordAddrOf` index check (`cc/addon.cc:623`), pins skip entries
without one (`cc/mutex.cc:338,437`).

---

## 4. Cross-reference of new findings

| C | Correctness | Performance | Security | Independent |
|---|---|---|---|---|
| C1 | T1 | — | — | reproduced |
| C2 | T2 | — | — | — |
| C3 | T3 (F13) | — | — | — |
| C4 | T4 | — | — | — |
| C5 | T5 | — | U1 | source |
| C6 | T10 | Q1 | — | source |
| C7 | T6 (R17) | Q3 (P5) | — | source |
| C8 | T8b | — | U2 | — |
| C9 | R19 | — | — | — |
| C10 | T8 | — | — | — |
| C11 | — | — | U3 | — |
| C12 | R6 residual | — | — | — |
| C13 | R15a | R15 | — | source |
| C14 | F35/R20 residual | — | — | — |
| C15 | T7, T9 | — | — | — |
| C16 | T11 | — | — | — |
| C17 | T12 | — | — | — |
| C18 | — | — | U4, U5 | — |
| C19 | — | Q2 | — | — |
| C20 | — | Q4 | — | — |
| C21 | — | Q5, Q7 | — | — |
| C22 | — | R22 | — | — |
| C23 | T14 | — | — | — |
| C24 | T15 | — | — | — |
| C25 | T13 | Q6, benchmarks rows | — | — |

---

## 5. New findings (merged)

| ID | Sev | Title | Location | Introduced by |
|---|---|---|---|---|
| C1 | P0 | Two `Mutex` instances on one thread share one native claim entry; closing or GC'ing one releases the other's lock | `cc/mutex.cc:372-410`; `src/mutex.ts:82-88,133-139` | H (`da6a2bc`) |
| C2 | P0 | Worker threads in one process publish into the same slot and steal each other's lock | `cc/mutex.cc:83,155-156,208` | O (`86faff1`) |
| C3 | P1 | Cross-process reclaim/takeover race publishes concurrent claimers into one slot (mutex and ring roles) | `cc/mutex.cc:180-191,520-533` | H |
| C4 | P1 | Ring role claim race between threads → two consumers; loser frees winner's slot | `cc/mutex.cc:539-551,567-569` | O |
| C5 | P1 | Row guard releases the attach row through a header pointer that a grow re-map unmapped | `cc/addon.cc:406-413,421,438`; `cc/segment.cc:369` | G (`23fc445`) |
| C6 | P1 | Async ring re-check compares a counter with itself → 250 ms stalls | `src/ringbuffer.ts:313-314,484-485` | I (`77e7412`) |
| C7 | P1 (macOS) | macOS waits are untimed; SDK header inside an anonymous namespace; availability macro unused | `cc/wait.cc:208-219,341-347,546-548` | `c39bd11` |
| C8 | P1 (Windows) | Windows `ReadHeader` row count not bounded by the mapped view | `cc/segment.cc:402-410` | pre-existing |
| C9 | P2 | R19 not fixed: failed identity read still cached per thread | `cc/liveness.cc:103-116` | — |
| C10 | P2 | `close()` does not disable `RingConsumer`/`Mutex` instances | `src/ringbuffer.ts:379-385`; `src/mutex.ts:133-139` | O |
| C11 | P2 | Init-loop retry paths skip the deadline → spin past `initTimeoutMs` | `cc/header.cc:360-363,400-405` | M (`b6c8b65`) |
| C12 | P2 | Takeover after a crashed grow rewrites kind and geometry | `cc/header.cc` takeover path | — |
| C13 | P3 | Parked-flag reset on role takeover never landed (claimed by commit I and ADR 0006) | ring open paths; `docs/adr/0006-ring-parked-flags.md:43-45` | — |
| C14 | P3 | Ring `peek`/`reserve({timeoutMs: NaN})` waits forever | `src/ringbuffer.ts` option handling | — |
| C15 | P3 | F20 row leak when the older Mapping detaches first; kind mismatch evicts the valid registry entry | `cc/registry.cc:59-62`; `cc/addon.cc:371-372` | L/N |
| C16 | P3 | Init-baton residuals (row count from shared `headerBytes` in a ns window; overflow judged dead; packed-word ABA) | `cc/header.cc:337-344,371-372` | M |
| C17 | P3 | Row leaks: failed takeover; creator that loses the CAS | `cc/header.cc:216-226,339-347` | — |
| C18 | P3 | `TryReuse` underflow if `mappingBytes` is 0 (Windows); `DataAddrOf` ignores byte offset; `mutexRegisterPin` takes pin and slot separately | `cc/addon.cc:287,734,835-838`; `cc/mutex.cc:279` | — |
| C19 | P3 | `reserveAsync` retries by throwing (~11 µs per failed attempt) | `src/ringbuffer.ts:300-304` | I |
| C20 | P3 | Full ring: consumer issues a FUTEX_WAKE per release until the producer runs (0.29–1.0/msg) | ring release path | pre-existing |
| C21 | P3 | Always-OR contended profile undocumented (≈1 wake per unlock, −14% at zero think time); every `posix_fallocate` error mapped to `E_NO_SPACE` | `src/mutex.ts`; `cc/segment.cc:275`, `cc/header.cc:151` | H |
| C22 | P3 | Registry reuse still shm_open + fstat + close (2.55–2.88 µs) | `cc/registry.cc:98-114` | B |
| C23 | P3 | Size-less join of a 0-byte object ftruncates it and costs ~250 ms + `initTimeoutMs` | `cc/segment.cc` grace path | — |
| C24 | P3 | Two `RingProducer` instances on one thread are allowed; interleaved async reserve/commit corrupts the ring | `cc/mutex.cc` role claim | by design |
| C25 | P3 | Documentation, PLAN and commit-message drift | see below | — |

### Details

**C1 — two instances share one claim (P0, reproduced).** `Mutex.open(name)`
twice on one thread yields the same mapping, slot and token, and the native
side keeps one claim entry matched by (data, slot). When either instance is
closed or collected by the FinalizationRegistry, `ReleaseHeldLock` releases
the lock the *other* instance holds and sets ownerDied. Reproduced in §2 with
`close()`; the correctness pass also reproduced it with natural GC and the
common open-a-Mutex-per-call pattern inside `withLockAsync`: 93 overlapping
critical sections in 8 s. Absent at `0f01571`, present at `da6a2bc`. **Fix:**
intern one JS instance per thread and segment, or keep one native entry per
instance with a refcount and release only a lock the collected instance
itself held.

**C2 — threads publish into one slot (P0, verified).** The claim marker is
per *process* (`cc/mutex.cc:155-156`), so a second thread sees its own pid
and proceeds; `PublishClaimedSlot` re-reads gen fresh (`:83`), so a second gen
CAS also succeeds; and a gen mismatch counts as dead (`:208`). 192/300 trials
had threads sharing a slot; with 8 workers locking through the public API,
13/40 trials overlapped (4,178 overlapping critical sections). 0 at both
`0f01571` and `da6a2bc`. Contradicts PLAN.md:355. **Fix:** CAS a per-thread
marker (pid+tid, or a reserved state), and CAS gen from the value seen when
the publish right was won.

**C3 — cross-process reclaim race (P1, verified; needs crash history plus
concurrent claimers).** The reclaim path reads gen and checks liveness, then
the publish re-reads gen (`cc/mutex.cc:180-191`); the ring dead-holder path is
the same (`:520-533`). With 64 dead slots, 30/30 trials had shared slots (up
to 7 processes on slot 0) and 10/10 end-to-end trials overlapped (4,538
overlaps). Ring with a dead holder and 6 concurrent openers: 4/30 trials gave
two producers, 2/30 two consumers. 0 at `0f01571`. **Fix:** the CAS's
expected gen must be the one read before the liveness verdict.

**C4 — ring role race between threads (P1, verified).** No branch handles a
pid equal to our own, so a second thread falls through to publish
(`cc/mutex.cc:539-551`), and the loser frees the winner's slot (`:567-569`).
6 workers opening a fresh ring: 4/200 trials gave two consumers; 1/200 left
the role word's gen different from the slot's (role stealable while alive).

**C5 — stale row-guard pointer (P1, gdb-verified + source).** The guard
captures `guard.h.base` right after `InitOrJoin`; `GrowSegment` and
`EnsureMappingCovers` can re-map the handle and munmap the old base. Any throw
after a winning grow — `dataBytes` above our cap or `st_size` before
`EnsureMappingCovers`, or the `wanted > maxWindow` check — runs
`~RowGuard`, which stores into the unmapped old header: 3/60 SIGSEGV
(backtrace `ReleaseAttachRow ← Open [clone .cold]`); if the address was
reused, silent corruption. Legitimate trigger: a peer with a larger
`MEMBRIDGE_MAX_SEGMENT_BYTES`; a same-uid writer can also force it. **Fix:**
the guard holds a reference to the handle and reads `handle.base` at unwind;
add a test that throws after a grow.

**C6 — racy async re-check (P1, measured; patch verified).** After the
non-blocking attempt fails, the code sets the parked flag and compares
`this.tail()` with itself, so a release that landed between the attempt and
the flag store (which saw the flag still 0 and did not notify) is missed; the
waiter parks on the new value until the next event or the 250 ms slice.
Measured: async reply side in a ping-pong 16–20 stalls per 5 s; tight async
producer 18–20 per 5 s; bursty stream 9–28 per 10 s. Snapshotting the
counter *before* the attempt and comparing against the snapshot gave 0 stalls,
ping-pong 445–603k round trips per 5 s, producer 2.15–2.30M msgs/s. The R14
test misses it (one message, 20 ms into the wait; its bound is 150 ms, not the
documented 50 ms).

**C7 — macOS untimed waits (P1 macOS, by reading).** `SyncWait` and the wait
thread call untimed `os_sync_wait_on_address` and re-check the deadline only
after a wake, so timeouts, the 250 ms dead-holder slices and initializer-death
checks never fire without a wake — a crashed holder hangs waiters forever.
Every wake also sleeps 50 µs. The SDK header is included inside an anonymous
namespace (`cc/wait.cc:208-219`) and `MEMBRIDGE_HAVE_OS_SYNC` never guards the
call sites, so pre-14.4 SDKs will not compile. `docs/compat.md` ("untimed
parks in bounded slices") is wrong. **Fix:**
`os_sync_wait_on_address_with_timeout` with a clockid (14.4+ SDK), or mark
macOS unsupported.

**C8 — Windows `ReadHeader` (P1 Windows, by reading, pre-existing).** The
Windows branch computes rows from `h->headerBytes` capped only by `maxAttach`
(up to 2048 rows / ~65 KiB), not by the view size, so `stat()`/`reap()` on a
small, raw or hostile section reads past the view. **Fix:** cap by
`VirtualQuery(base).RegionSize`, as POSIX caps by bytes read.

**C9 — R19 not fixed (P2, verified).** `cc/liveness.cc:103-116` records
pid/tid even when the read fails, so the intended retry never happens. After
fds are freed, `startTime` is still -1, `Mutex.lock` throws `E_SYSTEM`, and
attach rows stamped -1 show `'unknown'` (never reaped). The commit message and
code comment say failures are not cached.

**C10 — `close()` does not disable (P2, verified).** `RingConsumer.close()`
releases the role but `peek`/`release` have no closed check: a closed
consumer and its replacement both received message 7. `Mutex.close()` does
not poison the instance either, and closing while holding releases the lock
with ownerDied set.

**C11 — init loop ignores its deadline on retry (P2, by reading).** The
lost-CAS and dead-initializer handback paths (`cc/header.cc:360-363`,
`:400-405`) `continue` without checking the deadline; a writer that keeps
forging a dead baton keeps a sized opener spinning past `initTimeoutMs` with
the JS thread blocked. **Fix:** check the deadline every iteration.

**C12 — takeover after a crashed grow (P2, verified).** A joiner taking over
a crashed grow writes its own kind and geometry: mutex → plain, `dataBytes`
65536 → 4096 with the file still 69632 bytes; later mutex opens get
`E_INCOMPATIBLE`. **Fix:** a takeover keeps a valid prior header's kind and
geometry.

**C13–C25 (P3).**
- **C13:** commit `77e7412` touched no `cc/` file; nothing resets
  `CONSUMER_PARKED`/`PRODUCER_PARKED` on takeover, so after a crashed waiter a
  busy successor pays one FUTEX_WAKE per message (1.001/msg; 0.49–0.56 µs vs
  0.27–0.31 µs per message pair). ADR 0006:43-45 says otherwise.
- **C14:** ring `peek`/`reserve({timeoutMs: NaN})` waits forever (mutex
  rejects NaN now).
- **C15:** if the older Mapping detaches first it drops row ownership
  without handing it on, so the row leaks and `unlinkWhenUnused` never fires;
  a kind-mismatched open evicts the valid registry entry, causing duplicate
  Mappings and the same leak.
- **C16:** in the INITIALIZING branch the row count comes from shared
  `headerBytes`, which a takeover writes just after its CAS (a reader in that
  window judges a live initializer dead); with an overflowing attach table the
  bare-1 baton with slot -1 is judged dead; the packed word can ABA if `reap`
  frees a row a new takeover reuses.
- **C17:** a failed takeover (`EnsureSized` throws) never releases its row; a
  creator that loses the CAS keeps a fresh row with `owned=false`.
- **C18:** `TryReuse` computes `mappingBytes - headerBytes` without an
  underflow guard (reachable if Windows `VirtualQuery` fails);
  `DataAddrOf` ignores `ByteOffset`; `mutexRegisterPin` takes the pin SAB and
  slot as separate arguments. Internal bindings only — defense in depth.
- **C19:** each failed `reserveAsync` attempt builds a `MembridgeError` with
  a stack (10.4–11.5 µs vs 0.09 µs for a failed `peek`), widening C6's window.
  Use an internal try-reserve that returns null.
- **C20:** while the producer is parked on a full ring, the consumer wakes on
  every release until the producer runs (0.29–1.0 wakes/msg at saturation;
  same in both builds). `Atomics.exchange(flag, 0)` gives one wake per park.
- **C21:** always-OR gives ≈1 FUTEX_WAKE per contended unlock and 14% less
  throughput at zero think time (159–163k vs 186–189k acq/s) with barging
  tails of 47–166 ms — the right trade, but document it. Any
  `posix_fallocate` error (including EINTR) becomes `E_NO_SPACE`; seen 2/20
  runs under load with free space.
- **C22:** `NameRefersTo` still costs shm_open + fstat + close per reuse;
  `fstat(m->fd).st_nlink` detects unlink in one syscall (the name check is
  still needed for recreate).
- **C23:** a size-less open of a 0-byte object ftruncates it to 4096 and
  takes ~250 ms + `initTimeoutMs` (853 ms for 600 ms; 5.3 s at the default).
- **C24:** two `RingProducer` instances on one thread are allowed;
  interleaved async reserve/commit across them corrupts the ring.
- **C25 — drift:**
  - PLAN.md:397 says invalid `timeoutMs` throws `E_NAME_INVALID` and values
    over 2^31 are rejected; the code throws `E_SIZE_INVALID` and accepts
    them (guardrail 7: a PLAN contradiction is a bug).
  - PLAN.md:355 and ADR 0003:57 are contradicted by C2/C3.
  - ADR 0006:43-45 and commit `77e7412` claim C13's reset.
  - The R19 comment in `cc/liveness.cc` and commit `7497650` claim failures
    are not cached (C9).
  - `docs/compat.md` claims bounded macOS slices (C7) and a macOS
    `E_GROW_UNSUPPORTED` throw that does not exist.
  - `docs/benchmarks.md`: the 3-way contended row describes a residual
    commit H removed (and no bench produces it); the async row cites a 50 ms
    test bound (the test asserts 150 ms) and omits C6; the 64 KiB row is ~15%
    conservative; "~180k/s" is inconsistent with 6 µs; machine line says
    7.2.8 (host 7.2.9); the 64 B spread is understated.
  - `bench/mutex.js` still says recovery is dominated by a 250 ms slice
    (measures 0 ms).

---

## 6. benchmarks.md reproducibility (`npm run bench` ×6, load 2.3–2.6)

| Row | Doc | Measured | Result |
|---|---|---|---|
| `Atomics.add` | ~119 M ops/s | 86–119 | OK (best run matches; load) |
| Futex handoff | ~0.006 ms (~180k/s) | 0.006–0.007 ms, 153–171k/s | OK; "~180k/s" inconsistent |
| Uncontended lock+unlock | ~0.06 µs | 0.06–0.08 (direct loop 0.047–0.055) | OK |
| 3-way contended | "tight loops can still cost one wait slice" | 0 slice-bound waits in 31 runs | **Diverges** |
| SIGKILL → steal | ~0 ms | 0 ms ×6 | OK |
| Ring 64 B | ~2.9M | 2.0–3.8M | OK (spread understated) |
| Ring 4 KiB | ~317k, ~1.3 GB/s | 263–370k, 1.08–1.52 GB/s | OK |
| Ring 64 KiB | ~26k, ~1.7 GB/s | 26.7–31.8k, 1.75–2.09 GB/s | **Diverges** (~15% conservative) |
| Ring async | "ms-scale; R14 test < 50 ms" | test asserts < 150 ms; 250 ms producer stalls (C6) | **Diverges** |

---

## 7. Test gaps

| Gap | Test now | Adequate? |
|---|---|---|
| Ring counters at 2^31 / 2^32 | 2^31 only | Partly |
| Cross-process prefix open | R1 test | Yes |
| Worker churn: terminate, claim after unlink, GC release | "R11" locks *before* unlink; "R12" covers `close()` only | No |
| Create race with different sizes | R7 test has one joiner | No (fixed per repro) |
| Concurrent grow | R3/F8 tests sequential; F8 still spawns a stray `fork('-e')` | No |
| `lockAsync({signal})` over the cap; init vs a real live creator | none | No |
| Async ring latency | R14, `peekAsync` only, single message | Partly — misses C6 |
| `E_NO_SPACE` leaves no stuck name | not asserted; env-gated (skipped locally) | No |
| F12, F14 ordering, F20, R6 for a plain 0-byte object | none | No |
| **New:** two instances + close/GC (C1), concurrent slot claims across threads and processes (C2, C3), role claim race (C4), row-guard unwind after grow (C5), close-then-use (C10), R19 recovery (C9), ring NaN (C14), takeover after crashed grow (C12) | none | **No — every publish blocker in §1 is invisible to the 93-test suite** |

---

## 8. What is confirmed good

- Geometry: R1–R5, R8, R10 — cross-process prefix opens, concurrent
  create/join/grow with 0 spurious errors and 0 short or oversized windows,
  clean 15 s soak (256k joins, 9.7k touching joins, 0 SIGBUS).
- Init: R6 (0-byte), R7 (60 runs, single initializer), R9 (creator failure).
- Mutex: R11, R12, R13, F12, F14 — no teardown crash, no pin leak, 0
  slice-bound waits, correct ownerDied after unlink-then-lock.
- Ring: single async wakes in ms (R14 headline), F30.
- Performance: P1, P2, P3, P4, P7, P8, P9, P10, P11; no regressions in open,
  contention after bursts, sync ring path, or worker lifecycle.
