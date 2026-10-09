# Pre-release review — 2026-10-06 19:09 (+05:30)

| | |
|---|---|
| Commit reviewed | `e401d1c` (M8: full verification), clean working tree |
| Platform of evidence | Linux 7.2.8 (Fedora 43), x86_64, 18 cores, Node 24.18 |
| Reviews | 1. Edge-case & correctness · 2. Performance · 3. Security |
| Mode | Read-only. No repo file was modified by the reviews. PoCs and microbenches ran from scratch directories against the committed build; all used `/shm-bridge-test-review-*` or `/shm-bridge-sec-*` names and `/dev/shm` was verified clean afterwards. |

**How to read this.** Each review was performed by a separate agent with a
written brief; their reports are reproduced in §4–§6. "Verified" inside a
report means the reviewer reproduced it with a script on the machine above.
§2 lists the claims that were additionally re-checked against source while
assembling this document. macOS and Windows findings are from source reading
only — no toolchain for either was available.

---

## 1. Summary

The kernel-level primitives are sound and cost what they should (one
`sync.notify` ≈ 0.157 µs vs 0.120 µs for a raw FUTEX_WAKE). The defects are in
the layers above, and three of them are release-blocking classes:

1. **Header geometry is trusted.** A joiner sizes its `SharedArrayBuffer` from
   `header.dataBytes`/`headerBytes` and never checks them against the mapping
   or `st_size`. A hostile or merely racing segment yields an SAB larger than
   the mapping → cross-process OOB read/write, SIGBUS, SIGSEGV, V8 abort.
2. **Hangs in ordinary use.** Ring consumer deadlock after ~2 GiB of traffic
   (2^31 counter sign flip); mutex participant slots leaked by every exiting
   worker (table exhausted after 64); `waitAsync` promises never settling in a
   worker that reuses a dead isolate's address; a SIGKILLed zombie holder never
   stolen from.
3. **Init and identity races.** The creator never publishes `initState = 1`,
   so joiners take over a live creator; the per-process registry reuses a
   mapping after another process unlinked and recreated the name.

The macOS and Windows code paths do not compile as written, so the M8 "matrix
green" covers local Linux only.

### Release blockers (recommended order)

1. **Clamp every header-derived size to the real mapping** and fstat-check
   existing objects on every join; validate `headerBytes`/`initializerSlot`
   before indexing the attach table. Closes Sec F1–F5 and Correctness F6–F8,
   F29.
2. **Validate cached-mapping identity on reuse** (inode compare). Closes
   Correctness F16 / Sec F6, and Correctness F21.
3. **P0 hangs:** ring `(head - tail) >>> 0` (Corr F1), free slots on thread
   exit (Corr F2), erase dead hubs (Corr F3), treat zombies as dead (Corr F4).
4. **Init race** (Corr F5) and **mutex lost wakeups** (Corr F10 = Perf P2).
5. **Cheap high-value performance fixes:** clamp wait slice to remaining
   timeout (Perf P4), drop per-lock native tracking (Perf P1), ring parked
   flag (Perf P3 — decide before the layout is frozen).
6. **Make macOS/Windows compile** (Corr F19) and actually run those CI legs
   before any compat claim; fix the macOS untimed async wait (Perf P5).

Per AGENTS.md guardrail 7, fixes that change design go PLAN.md → code → ADR.

### Finding counts

| Review | Findings | Cleared |
|---|---|---|
| Correctness | 38 (P0:4 P1:15 P2:11 P3:8) | 17 |
| Performance | 11 (+1 correctness note) | 7 axes |
| Security | 8 (P0:4 P1:2 P2:2) | 7 |

---

## 2. Independent source checks

Re-checked against `e401d1c` while assembling this document:

| Claim | Evidence |
|---|---|
| Ring consumer uses signed `head - tail < framed` (Corr F1) | `src/ringbuffer.ts:311` |
| `MutexReleaseThreadSlots` is defined/declared but never called (Corr F2) | `cc/mutex.cc:192`, `cc/mutex.h:58`; no other references in `cc/` or `src/` |
| Liveness is `kill(pid,0)` + optional start-time match, no zombie state check (Corr F4, Sec F7) | `cc/liveness.cc:104-110` |
| `mutexTrackHeld` on every acquire path, `mutexUntrackHeld` on every unlock (Perf P1) | `src/mutex.ts:118,140,175,194,210,251,258` |
| Wait loops pass constant `sleepSliceMs = 250` (Perf P4) | `src/mutex.ts:33,154,220` |
| Ring notifies unconditionally on commit/release (Perf P3) | `src/ringbuffer.ts:214,335` |
| Nothing wakes the `initState` word joiners wait on (Perf P10) | `cc/header.cc:232`; no `SyncWake` in `cc/header.cc` |
| Size-less join window = header `dataBytes`, unclamped (Sec F1, Corr F7) | `cc/addon.cc:389-396` |
| Sized join only truncates when file < header page; no `>= header + requested` check (Sec F2) | `cc/segment.cc:211-218` |
| Takeover indexes `attachTable[initSlot]` bounded only by header-derived `AttachSlotCount(h->headerBytes)` (Sec F3) | `cc/header.cc:193-198` |

**Citation corrections to the security report:** it cites
`cc/liveness.cc:533-548` and `cc/registry.cc:286-296`, which are out of range
(the files are 195 and 127 lines). The liveness logic is at
`cc/liveness.cc:104-110`; the registry name lookup is `Registry::Find` at
`cc/registry.cc:88`. The corrected references are used in §6.

---

## 3. Cross-review map

| Theme | Correctness | Performance | Security |
|---|---|---|---|
| Header geometry / `st_size` trust on join | F6, F7, F8, F29 | — | F1–F5 |
| Init race (`initState = 1` never written) | F5 | noted | — |
| Registry reuse after unlink + recreate | F16 | — | F6 |
| Mutex lost wakeups | F10 | P2 (measured 14k → 40k acq/s) | — |
| Zombie / unreadable `/proc` liveness | F4, F25 | — | F7 |
| Ring 2^31 counter wrap | F1 | — | (cleared within a correctly sized SAB) |
| Worker churn: slot leak, hub reuse | F2, F3 | — | — |
| Async waiter cap 191 per process | F11, F37 | P6 | — |
| macOS / Windows | F19 (no compile), F27, F28 | P5 (macOS untimed async wait) | F8 (case-insensitive names) |

---

## 4. Review 1 — Edge-case and correctness

### 4.1 Verdict

The single-word token design, the M1 futex facts and the basic open/registry
paths hold up. But the build has several bugs that hang or crash in ordinary
use:

- **Ring:** the consumer hangs forever once about 2 GiB has flowed through a
  ring.
- **Mutex:** participant slots leak permanently every time a worker thread
  exits, and a SIGKILLed holder that is still a zombie is never stolen from.
- **Async waits:** a later worker whose isolate gets the same address as a
  dead one has its `waitAsync` promises never settle.
- **Init:** the init protocol never protects a live creator, because the
  creator never writes `initState = 1`.

The test suite misses all of these: no 2^31 counter test, the steal test waits
for the child to be reaped, the slot-exhaustion test fakes dead pids, and
there is no worker-churn test. The macOS and Windows code cannot compile as
written.

### 4.2 Findings

| ID | Sev | Title | Location |
|---|---|---|---|
| F1 | P0 | Ring consumer hangs forever at the 2^31 head/tail sign flip | src/ringbuffer.ts:311 |
| F2 | P0 | Mutex slots of exited workers are never freed → table exhausted for every process | cc/mutex.cc:72-121, 192 (unused), 221 |
| F3 | P0 | `g_hubs` never erased → `waitAsync`/`lockAsync` hang in a worker reusing a dead Isolate* | cc/wait.cc:521-536, 585-631 |
| F4 | P0 | Zombie holder reads as alive → sync `lock()` in its parent deadlocks | cc/liveness.cc:104-110 |
| F5 | P1 | Creator never sets initState=1 → joiners take over a live creator | cc/header.cc:145-154, 205-216; cc/segment.cc:215, 227 |
| F6 | P1 | Size-less join of a zero-length (mid-create) object → SIGBUS | cc/segment.cc:206-210; cc/header.cc:167 |
| F7 | P1 | Size-less join window comes from header `dataBytes`, not the mapping → out-of-mapping SAB | cc/addon.cc:390-402, 273 |
| F8 | P1 | Losing grower's 0 return is ignored → SAB past EOF; a late grower shrinks header `dataBytes` | cc/addon.cc:376-385; cc/header.cc:259, 283 |
| F9 | P1 | Huge finite timeouts: sync wait returns immediately; async mux spins at 100% CPU and starves untimed waits | cc/wait.cc:77-78, 202-203, 425-426, 383; src/sync.ts:51 |
| F10 | P1 | Lost wakeup: acquiring CAS clears HAS_WAITERS; a waiter sleeps on a free-with-bit word | src/mutex.ts:115, 148-154, 191, 216-220 |
| F11 | P1 | `lockAsync({signal})` crashes the process on E_TOO_MANY_WAITERS (unhandled `.finally` rejection) | src/mutex.ts:226 |
| F12 | P1 | gen15 wrap gives slot 0 token 0 → mutual exclusion broken; tryLock throws E_DEADLOCK on a free lock | cc/mutex.cc:118, 323 |
| F13 | P1 | Slot reclaim uses ABA-prone state CAS (PLAN says gen CAS) plus a stale lock snapshot → permanent deadlock | cc/mutex.cc:84-101, 133 |
| F14 | P1 | Held-lock/role pin lookup fails after `unlink` → dead worker's lock never released | cc/mutex.cc:166, 237, 342 |
| F15 | P1 | reap / unlinkWhenUnused race against joiners (and the creator) → split brain | src/ops.ts:171-183; cc/registry.cc:50-81; cc/header.cc:147-149 |
| F16 | P1 | Registry reuses a mapping whose name was unlinked and recreated elsewhere → split brain in-process | cc/addon.cc:335-341 |
| F17 | P1 | `PromiseState::context` Global never reset → destroyed off-thread or after isolate disposal | cc/wait.cc:541 (+ wait.h PromiseState) |
| F18 | P1 | `uv_async_send` on a closed hub from the fallback waiter thread | cc/wait.cc:309, 187 |
| F19 | P1 | macOS/Windows cannot compile | cc/wait.cc:35, 224 vs 470, 286 vs 461; cc/header.h:69 |
| F20 | P2 | Attach row owned only by the first Mapping → in-use segment unlinked | cc/addon.cc:410; cc/registry.cc:57-75 |
| F21 | P2 | unlinkWhenUnused unlinks the *name*, which may now be someone else's segment | cc/registry.cc:75 |
| F22 | P2 | `ownerDied` reported twice after a steal | src/mutex.ts:137, 207 |
| F23 | P2 | Ring roles cannot be released or re-claimed by the same thread; crash while Reserved → infinite hot loop | cc/mutex.cc:265-270, 300 |
| F24 | P2 | `stat`/`reap` read only 128 attach rows; 16K/64K pages have 511/2047 → live segment reaped | src/ops.ts:83; cc/addon.cc:751 |
| F25 | P2 | startTime=-1 written when /proc read fails → live owner later judged dead and stolen from | cc/liveness.cc:86-95, 104-110 |
| F26 | P2 | Grower never records itself as initializer: takeover during a live grow; grower crash bricks segment | cc/header.cc:193-216, 250-258 |
| F27 | P2 | Windows: `mode:'join'` creates; UNLINKED flag never checked by joiners | cc/segment.cc:119-124; cc/header.cc:169 |
| F28 | P2 | Windows default notify releases INT_MAX permits → waiter hot-spin (unproven) | cc/addon.cc:589; cc/wait.cc:505 |
| F29 | P2 | ReadHeader reads uninitialized stack when the file is shorter than `headerBytes` | cc/segment.cc:323-350 |
| F30 | P2 | §8.2 gaps: no reserveAsync/peekAsync; maxMessage never checked; producer join unvalidated | src/ringbuffer.ts:106-132 |
| F31 | P3 | Raw `grow` can shrink a concurrent raw grower's file | cc/segment.cc:190-196 |
| F32 | P3 | Mux EAGAIN path dereferences cancelled nodes; cancel does not bump `g_ctrl` (unproven) | cc/wait.cc:366-374, 585-610 |
| F33 | P3 | No kind check on join; registry reuse ignores raw/kind/unlinkWhenUnused | cc/header.cc:169-187; cc/addon.cc:335 |
| F34 | P3 | `stat()` liveness is a bare `kill(pid,0)`; Windows always "alive" | src/ops.ts:96-107 |
| F35 | P3 | Timeout input hygiene: initTimeoutMs int64 UB, wall-clock deadlines, NaN = infinite | cc/header.cc; src/mutex.ts:107, 185; src/ringbuffer.ts:163, 288 |
| F36 | P3 | Fallback divergences that tests would not catch | src/fallback.ts:221-233 |
| F37 | P3 | Contract nits: waitAsync never 'not-equal'; E_TIMEOUT for slot exhaustion; transient E_TOO_MANY_WAITERS | cc/wait.cc:366-374, 558; cc/mutex.cc:121 |
| F38 | P3 | Second `setShmBridgeErrorCtor` in one isolate may abort via duplicate cleanup hook (unproven) | cc/addon.cc:78 |

### 4.3 Details

**F1 (P0, verified).** `peek()` checks `head - tail < framed` on raw signed
i32 values. When `head` crosses 2^31 and goes negative while `tail` is still
positive, the difference is about −2^32, so the consumer treats the message as
"not fully committed" and waits forever. This happens whenever a message is
still unread across the 2^31 boundary; a SKIP marker crossing is harmless.
Repro: preset head = tail = 2147483632, write a 5-byte message;
`read({timeoutMs:600})` returns null with the message present. Impact: the
producer fills the ring and both sides deadlock after about 2 GiB of total
traffic (≈1 s at the 2.1 GB/s in docs/benchmarks.md). No test exercises the
crossing. Fix: `((head - tail) >>> 0) < framed`; add tests presetting counters
near 2^31 and 2^32.

**F2 (P0, verified).** A slot returns to Free only via ReleaseHeldLock, i.e.
when a thread dies *holding* the lock. `MutexReleaseThreadSlots` is never
called. A worker that locked, unlocked and exited keeps its slot Active
forever; reclaim checks only process liveness, and the process is alive.
Repro: 64 sequential workers each lock/unlock; the 65th worker and the main
thread get `E_TIMEOUT: no mutex participant slot available`. Permanent for
every process sharing the mutex; PLAN §7.2 says the hook frees the slot on
graceful exit. The exhaustion test uses fake dead pids. Fix: register the
env-cleanup hook at claim time and free the thread's slots in it.

**F3 (P0, verified).** `CancelIsolateWaits` closes the hub's uv handle but
never erases `g_hubs[isolate]`. A later worker whose `v8::Isolate*` lands at
the same address gets the old hub (`closed=true`, dead loop); Deliver never
sends, so the promise never settles, and the cleanup hook is never registered
for the new isolate. Repro: 8 sequential workers each run
`waitAsync(..., 100)`; workers 1, 3, 4, 5, 6, 7 hung for over 2 s. Impact:
`lockAsync` hangs forever; `pinLoopWhile` keeps that worker alive. Fix: erase
the hub in the cleanup hook; keep the closed handle's memory alive via a
`uv_close` callback owning a `shared_ptr`.

**F4 (P0, verified).** `kill(pid,0)` succeeds on a zombie and
`/proc/<pid>/stat` still exists with the same starttime, so the holder reads
alive. A Node parent reaps children only when its event loop runs, so a parent
blocked in sync `lock()` never reaps the dead holder it spawned. Containers
whose PID 1 does not reap orphans are stuck permanently. Repro: child locks and
is SIGKILLed; parent's `lock({timeoutMs:1500})` → E_TIMEOUT, state field `Z`;
after the loop reaps, the steal works. The steal test awaits `'exit'` first,
hiding this. Fix: parse field 3 of `/proc/<pid>/stat`, treat `Z`/`X` as dead;
`proc_pidinfo` on macOS.

**F5 (P1).** The creator writes the header, claims its row and stores ready
(2) but never stores 1. A joiner arriving between the creator's
`shm_open(O_EXCL)` and its final store sees state 0 and takes over with no
liveness check (header.cc:205):

1. C: `shm_open` succeeds (size 0).
2. J: `fstat` = 0 → truncates to H+Jsize (segment.cc:215), maps, takes over,
   stores ready, returns a Jsize SAB.
3. C: `ftruncate(H+Csize)` — if Csize < Jsize this shrinks the file under J —
   then writes `dataBytes = Csize`.

Consequences: J's SAB SIGBUSes after the creator's truncate (verified,
simulated); two `exact` opens with different sizes both succeed; on macOS (U2)
the second ftruncate fails, so the creator throws and `shm_unlink`s the object
J is using. The "live initializer → wait" branch is only reachable via
GrowSegment or the hand-crafted header in init-recovery.test.ts:89. Fix:
creator CASes 0→1 with its `initializerSlot` before sizing; joiners never
truncate a non-ready object; state 0 on a recent object means wait, not take
over.

**F6 (P1, verified).** For a size-less join of an object with
`st_size < headerBytes`, `total` becomes `headerBytes` with no ftruncate;
`InitOrJoin` loads `initState` from a page past EOF → SIGBUS.
`RingConsumer.open` always joins size-less, so a consumer retrying on
E_NOT_FOUND while the producer is being created crashes. Fix: if
`fileSize < headerBytes` on a size-less join, wait or throw; never map past
EOF.

**F7 (P1, verified with a crafted header).** In the size-less path
`windowBytes` and `m->dataBytes` come from header `dataBytes`, while
`mappingBytes` comes from the earlier `st_size`. Real race: joiner fstat+mmap,
then a grower truncates up and publishes a larger `dataBytes`, which the
joiner reads → SAB longer than the mapping. `TryReuse`'s guard inherits the
wrong value. Repro: `dataBytes` = 64 MiB on an 8 KiB object → 64 MiB SAB,
SIGBUS on touch. Fix: clamp to `mappingBytes - headerBytes` or retry; always
set `m->dataBytes` from the mapping. (Deepened by Security F1.)

**F8 (P1).** GrowSegment returns 0 when it loses the init-lock race; Open
ignores it and the grow policy has no post-check. (1) A grows to 2 MiB while
B, wanting 3 MiB, waits; B gets 0 and returns a 3 MiB SAB over a 2 MiB file →
SIGBUS. (2) B's `requested > existing` check uses a snapshot; if A grew to
3 MiB meanwhile, B takes the lock and writes `dataBytes = 2 MiB`, shrinking the
header below A's SABs. Fix: loop the policy on 0; inside the lock write
`max(current, new)`.

**F9 (P1, verified).** `sync.ts` only rejects non-finite timeouts, so 1e19 or
1e300 pass; `tv_sec = (time_t)(ms/1000)` overflows and `tv_sec*1000`
overflows int64. Sync wait: EINVAL → `'timed-out'` after 0 ms. Mux: EINVAL
falls into "re-snapshot" and spins (2001 ms CPU in 2000 ms); other async waits
get neither wakeups nor timeouts while that wait holds the minimum deadline (an
untimed wait notified at 300 ms did not resolve within 2 s). A process exiting
during the spin returned status 1 despite `process.exit(0)` (cause
unconfirmed). Fix: treat ≥ 2^53 as infinite, compute nsec with fmod, handle
EINVAL in the mux.

**F10 (P1, verified).** The acquiring CAS writes a bare `token`, clearing
HAS_WAITERS while others are parked; the next unlock sees no bit and doesn't
notify. A waiter that ORs the bit into an already-free word then sleeps on
`0x80000000` while the lock is free. Measured: with three waiters, the second
and third acquired +94 to +100 ms after the first (slice timeout, not notify);
worst case 250 ms per handoff. Mutual exclusion holds; liveness and latency do
not. Fix: acquire with `token | HAS_WAITERS` once a thread has waited; retry
immediately when `Atomics.or` returns a free word. (Quantified in Perf P2.)

**F11 (P1, verified).** `waitP.finally(...)` creates a second promise. When
`waitAsync` rejects with E_TOO_MANY_WAITERS (cap 191 = 127 mux + 64 threads),
that derived promise is unhandled and the process dies — sync.ts:26 warns
about exactly this. Repro: 191 waits pending, then `lockAsync({signal})`: the
caller's catch fires, then the process exits 1. Fix: `waitP.then(cleanup,
cleanup)`.

**F12 (P1, verified by crafting).** Token = `(slot<<16) | ((g+1) & 0x7FFF)`.
For slot 0 every 32768th claim yields token 0. Slot 0 absorbs most churn
(index-order scan reclaims dead slots first), so ~32k short-lived processes
reach it. Then `lock()` "acquires" via CAS 0→0 so others also acquire, and
`tryLock()` on a free mutex throws E_DEADLOCK. The ring producer role (slot 0)
has the same issue: role word 0 reads "free". Fix: skip gen values producing
token 0.

**F13 (P1, interleaving spelled out, not reproduced).** PLAN §7.2 says claim
by CAS on `slot.gen`; the code CASes `state` (Active→Reserved), which is
ABA-prone. (A) R reads slot s Active with dead owner D and runs CheckLiveness
(tens of µs); Q reclaims s meanwhile; R's CAS still succeeds, overwrites Q's
identity and bumps gen; Q's cached token is stale; if Q locks and dies,
`MutexOwnerAlive` says alive (gen mismatch, mutex.cc:133) → permanent
deadlock. (B) R snapshots `currentLock = 0`; D locks and is SIGKILLed during
R's scan; R reclaims D's slot; the lock word holds a stale-gen token forever.
Fix: CAS on gen, re-read the lock word after reserving, treat stale-gen tokens
as stealable.

**F14 (P1, verified).** `MutexTrackHeld`/`MutexTrackRole` find the pin via
`Registry::FindByAddress`, which only searches live registry entries;
`unlink(name)` erases the entry, so the pin is null and the cleanup hook skips
the release (mutex.cc:237). Repro: main and a worker open the mutex, main
unlinks, the worker locks and is terminated; main's lock gives E_TIMEOUT
forever (process alive, so no steal). Fix: take the pin from the SAB's
BackingStore deleter data, not the registry.

**F15 (P1, interleaving).** `reap()` is `stat()` (pread snapshot + `kill` per
row) then `unlink`, with no lock or tombstone. Joiner: J opens and claims a row
after R's snapshot; R unlinks; J keeps using the orphan; the next opener
creates a new segment. Creator: between `WriteHeader` (magic visible,
header.cc:147) and `EnsureAttachedRow` (149) the table is empty, so a reaper
unlinks a segment whose creator is mid-open (the window is magic→row, not
ready→row). `Mapping::Detach` (unlinkWhenUnused) has the same race; its
comment claiming a magic check is false. Fix: tombstone state (CAS
ready→reaping, re-scan rows, unlink); joiners re-check state after claiming a
row; creator claims its row before writing magic.

**F16 (P1, verified).** `TryReuse` keys on name only. If another process
unlinks and recreates the name, `open(name)` returns the old mapping (this
process saw 111, a fresh process saw 222). Triggered by any long-lived process
plus `reap()` or an operator unlink. Fix: compare `fstat(m->fd).st_ino` with a
fresh stat before reuse; drop stale entries. (Also Security F6.)

**F17 (P1, by reasoning).** `PromiseState.context` is never cleared;
`ResolveOnLoop` clears only `resolver` and `buf`. The last
`shared_ptr<PromiseState>` is often dropped on the mux or fallback thread,
running `~Global<Context>` off the isolate thread (data race on V8 global-handle
lists) or after a terminated worker's isolate is gone (use-after-free).
wait.h itself says Globals must not be destroyed off-isolate. Fix: reset
`context` in `ResolveOnLoop` and `CancelIsolateWaits`.

**F18 (P1 on the thread path, by reasoning).** `WaitThreadMain` sends
`uv_async_send(&hub->async)` unconditionally, even after `CancelIsolateWaits`
`uv_close`d it and the loop is freed — libuv reads freed `handle->loop` and
writes to a possibly reused fd. Deliver has a narrower version (send decided
under `hub->mu`, sent after unlocking). The thread path is always used on
macOS/Windows and on Linux beyond 127 waits or below kernel 5.16. Fix: check
`closed` and send while holding `hub->mu`.

**F19 (P1, definite from source; no mac/win toolchain).**
`cc/wait.cc:35` includes `<unistd.h>` unconditionally (no MSVC header);
`WaitThreadMain` uses `WordSemaphoreName` (line 224) before its static
definition (line 470); the macOS branch uses `os_sync_wait_on_address` /
`OS_SYNC_WAIT_ON_ADDRESS_SHARED` (line 286) declared only at 455-461 with no
SDK header; `cc/header.h:69` calls `sysconf(_SC_PAGESIZE)` in every TU
including Windows. All U1–U3 runtime claims are therefore untested. Fix: move
declarations up, guard POSIX includes, `GetSystemInfo` on Windows, run the CI
legs.

**F20 (P2, verified).** Only the Mapping that claimed the attach row releases
it. After `open(n, 4096, {unlinkWhenUnused:true})` and a grow to 8192, a
second Mapping shares the row; once the first SAB is GC'd, the row is released
and the name unlinked while the grown SAB is in use. `reap()` also sees it as
unattached. Fix: per-process refcount of the row keyed by inode.

**F21 (P2).** `Detach` calls `shm_unlink(name)` without checking the name
still refers to its object; after unlink+recreate the last detacher of the old
object deletes the new one. Fix: compare inodes first.

**F22 (P2, verified).** The steal path stores `OWNER_DIED = 1`, returns
`ownerDied: true`, and leaves the word set, so the next acquire's
`exchange(OWNER_DIED, 0)` reports true again (measured: true, true, false).
Fix: don't store 1 on the steal path.

**F23 (P2).** No producer/consumer close API — roles free only on thread
teardown or process death. The same thread cannot re-open a role it holds
(nonzero role word → `MutexOwnerAlive(self)` → E_ROLE_TAKEN before the
own-slot branch). If a claimer dies while the slot is Reserved and the role
word is 0, later claimers `continue` forever (100% CPU, JS thread blocked, no
timeout). Fix: `close()`, own-token handling, bounded liveness-checked recovery
of Reserved.

**F24 (P2).** `readHeader(name, 128)` drops rows ≥ 128. With 16 KiB pages
(macOS arm64) there are 511 rows, with 64 KiB pages 2047; live attachers end
up in high rows after churn and `reap()` unlinks a live segment. Fix: read all
`AttachSlotCount(headerBytes)` rows.

**F25 (P2).** `SelfIdentity` stores `startTime = -1` when
`fopen("/proc/self/stat")` fails (EMFILE under load). `CheckLiveness` later
reads the real starttime, sees a mismatch, returns kDead → live holder stolen
from, breaking mutual exclusion. Fix: refuse to claim with incomplete
identity; treat −1 as unknown.

**F26 (P2).** GrowSegment takes the init lock (2→1) without setting
`initializerSlot`; joiners judge the old creator's row. If the creator has
detached or died, a joiner takes over mid-grow and writes its own `dataBytes`
(an `exact` joiner with a larger size even truncates up). If the grower
crashes while the creator lives, every joiner gets E_INIT_TIMEOUT forever.
Fix: record the grower as initializer.

**F27 (P2, Windows, runtime unverified).** `OpenSegment` calls
`CreateFileMappingW` for `mode:'join'` too, so a sized join of a missing name
creates a section (§5.1 requires E_NOT_FOUND); a size-less join gives
E_SYSTEM. `InitOrJoin` never checks `kFlagUnlinked`, so Windows `unlink`
doesn't prevent new joins (§5.4). Fix: `OpenFileMappingW` for join; check
UNLINKED in the ready branch.

**F28 (P2, Windows, unproven).** Default notify count 2147483647;
`ReleaseSemaphore` on an idle semaphore saturates it, so later async waits on
that word return immediately and re-loop (hot spin) until permits drain. Fix:
cap releases at a tracked waiter count.

**F29 (P2).** On POSIX, `ReadHeader` derives row count from the file's
`headerBytes` (capped at 64 KiB), not from bytes actually read; a short file
means rows are copied from uninitialized stack. Fix: bound by `total`.

**F30 (P2, spec).** §8.2's `reserveAsync`/`peekAsync` are not implemented; a
joiner's `maxMessage` is never compared with the header (§8.2 requires
E_SIZE_MISMATCH); `RingProducer.open` without a capacity doesn't validate the
power-of-two shape.

**F31 (P3).** Raw `grow` has no lock: two growers both `fstat` the old size;
the smaller truncate lands last and shrinks the file under the larger mapping.

**F32 (P3, unproven).** The EAGAIN loop dereferences `*snapshot[i]->addr`
even for nodes just cancelled; cancel doesn't bump `g_ctrl`, so a lost
`WakeMux` keeps a stale entry. A segfault needs an unmap in a tiny window.

**F33 (P3).** Join never checks the kind bits despite core.ts:26 documenting
E_INCOMPATIBLE. Registry reuse ignores `raw`: opening a name raw after a plain
open (or vice versa) can return a window at offset 0 that includes the header
page (guardrail 4).

**F34 (P3).** `stat()` liveness ignores starttime and zombies, and on Windows
always reports alive — diverges from §7.1's CheckLiveness.

**F35 (P3).** `initTimeoutMs` of Infinity or 1e300 → int64 cast UB; mutex and
ring deadlines use `Date.now()` (wall-clock jumps); NaN `timeoutMs` silently
means forever.

**F36 (P3).** Fallback `mode:'join'` creates instead of throwing E_NOT_FOUND;
its map is per isolate so workers don't share. Mutex and RingBuffer always
refuse the fallback, but §5.6 says "unless explicitly opted in".

**F37 (P3).** Async waits deliver `'ok'` on EAGAIN, so §6's `'not-equal'`
never appears; slot exhaustion reuses E_TIMEOUT; `g_pending.size()` counts
unpruned cancelled nodes → spurious E_TOO_MANY_WAITERS.

**F38 (P3, unproven).** Node likely CHECKs that a cleanup hook (fn, arg) pair
is unique, so a module-registry reset (e.g. jest) re-running the loader in one
isolate could abort. Not confirmed against Node source.

### 4.4 Cleared

1. Producer counter math (`used = (head-tail)>>>0`, `pos`, `align8`, SKIP) is
   correct across 2^31 and 2^32; `maxMessage ≤ cap/2 − 8` keeps SKIP possible
   on a drained ring; a SKIP exactly at 2^31 was handled correctly.
2. Producer dying between length store and head store: nothing becomes
   visible; the next producer overwrites the slot (matches §8.2).
3. GrowSegment's `compare_exchange_weak` loop handles spurious failure
   correctly.
4. An `at-least` opener after a grow maps only `requested ≤ nowExisting`.
5. `TryReuse` clamps correctly for mappings created by a sized open (only
   size-less-created mappings are wrong, F7).
6. Lock ordering: only g_mu→hub->mu, g_held_mu→Registry::mu_,
   g_held_mu→g_detach_mu; no reverse nesting, no cycle.
7. Double delivery prevented by `delivered.exchange` + `settled.exchange`.
8. tv_nsec stays < 1e9 for in-range values; `steady_clock` is
   CLOCK_MONOTONIC on Linux; 0.1 ms and 2^53 ms are fine.
9. Raw mode: every accepted join maps at most `st_size`.
10. `pinLoopWhile` released on both settle arms; abort listeners removed
    (apart from F11).
11. Two Mutex instances on one thread follow §7.3's re-entrancy rule.
12. Unlock-after-steal detects the foreign token and throws E_NOT_OWNER.
13. Steal CAS preserves HAS_WAITERS; starttime PID-reuse check works.
14. Tokens never collide with bit 31 (max token 0x3F7FFF).
15. `list()`/`stat()` are TOCTOU-tolerant.
16. futex_waitv capability probe (EAGAIN vs ENOSYS) is correct.
17. A consumer dying mid-peek is redelivered (at-least-once, documented).

### 4.5 Test gaps

Ring counters near 2^31 and 2^32 · steal without reaping the child first ·
worker-churn mutex slots · `ownerDied` on the second acquire · worker-churn
`waitAsync` · create race with different sizes · concurrent grow ·
`lockAsync({signal})` over the async-wait cap · init test against a real live
creator (the current one hand-crafts initState=1).

---

## 5. Review 2 — Performance

`npm run bench` was run twice and matched docs/benchmarks.md: Atomics.add
~116 M ops/s, handoff ~2 µs, lock+unlock 0.18–0.20 µs, ring 64 B
~2.3 M msgs/s, 64 KiB ~2.0 GB/s.

### 5.1 Verdict

The kernel primitives cost what they should: one `sync.notify` is 0.157 µs vs
0.120 µs for a raw shared FUTEX_WAKE in C; a real sleep/wake ping-pong between
processes is ~4.5 µs. The losses are in the layers above:

- **Mutex lock/unlock** makes a native bookkeeping call that takes a
  process-wide lock and scans every live mapping — ~75% of the uncontended
  cost, and it collapses with several locking threads.
- **Contended Mutex** has two lost-wakeup paths that make waiters sleep a full
  250 ms slice while the lock is free.
- **Ring** pays a FUTEX_WAKE per message even with nobody waiting.
- **Timeouts** use a fixed 250 ms slice, so short `timeoutMs` overshoots to
  250 ms.

Top 3: P1 (lock+unlock 0.18 → ~0.04 µs; 8-thread case ~200×), P2 (contended
14k → ~40k acq/s, no 250–750 ms stalls), P3 (zero-copy small messages
~2.5M → 3.6–4.6M msgs/s).

### 5.2 Findings

| ID | Impact | Title | file:line | Cost |
|---|---|---|---|---|
| P1 | hot-path + scalability | Per-lock native held-lock tracking: global `g_held_mu` + O(live mappings) `FindByAddress` per lock, untrack per unlock | cc/mutex.cc:163-190, cc/registry.cc:105-125, src/mutex.ts:118,140,175,194,210,258 | **Measured:** 0.13 of 0.18 µs; 1.54 µs @100 mappings, 6.7 µs @500; 8 threads 8.4 µs/op (49×) |
| P2 | latency / throughput | Waiter sleeps on a free lock word; acquiring after a wait drops HAS_WAITERS | src/mutex.ts:150-154, 219-220, 115, 191 | **Measured:** 14.2k vs 39.6–41.5k acq/s; max wait 750 ms vs 5–185 ms |
| P3 | hot-path | Ring `commit()`/`release()` always FUTEX_WAKE | src/ringbuffer.ts:214, 335 | **Measured:** +40–80% msgs/s small zero-copy |
| P4 | latency | Fixed 250 ms slice ignores remaining timeout | src/mutex.ts:154, 220; src/ringbuffer.ts:195, 294, 314 | **Measured:** `lock({timeoutMs:1})` → 250.4 ms; `peek({timeoutMs:1})` → 250.2 ms |
| P5 | latency / CPU (macOS, Windows) | Sync waits poll 50 µs→2 ms on non-Linux; macOS async wait untimed; Windows notify opens semaphore by name per call | cc/wait.cc:436-447, 283-286, 501-507, 470-488 | **Simulated:** +1.6 ms mean wake latency (p99 3.3 ms); 6.5% core per 64 waiters. Windows estimated |
| P6 | scalability | One process-wide async cap of 191; mux rebuilds whole wait set per register and per wake | cc/wait.cc:135-145, 314-352, 556-570 | **Measured:** 250 `lockAsync` → 59 × E_TOO_MANY_WAITERS; round trip 7.7 → 39.6 µs at 126 parked |
| P7 | CPU (contended) | Liveness reads /proc on every contended pass; own identity re-read every time | cc/liveness.cc:86-111; src/mutex.ts:129, 204 | **Measured:** 3.7 µs/check, 1.0–1.5 per contended acquisition; `selfIdentity` 4.6 µs |
| P8 | scalability (per instance) | Slot claim liveness-checks every active slot before the first free one | cc/mutex.cc:86-97 | **Measured:** 5.1 µs + ~3.1 µs per live slot (197 µs @63) |
| P9 | ops (low) | `readHeader` materializes all ~126 rows; stat() drops empties | cc/addon.cc:765-778, src/ops.ts:91-92 | **Measured:** 98.7 µs vs 4.1 µs; `reap()` over 500 segments 60 ms |
| P10 | latency (rare) | Writes of initState=ready never wake joiners | cc/header.cc:154, 215, 272-284 vs 232 | **Reasoned:** up to 250 ms during concurrent grow |
| P11 | bench fidelity | contention bench's pong side busy-spins | bench/contention.js:62-66 | **Measured:** true both-sides-sleep ping-pong 4.48–4.54 µs |

### 5.3 Details

**P1 — per-lock native tracking.** Every successful `lock()`/`tryLock()`
calls `mutexTrackHeld`: takes `g_held_mu`, looks up the isolate, calls
`Registry::FindByAddress` → `Live()`, which takes the registry mutex, walks
every entry locking each `weak_ptr` (two atomics per mapping on shared cache
lines) and allocates a vector. Every `unlock()` takes `g_held_mu` again.
Measured: atomics only 0.038 µs; track+untrack 0.125–0.139 µs; full
lock+unlock 0.188 µs; 1.54 / 6.7 µs with 100 / 500 live mappings; 1/2/4/8
threads each locking their own mutex: 0.17 / 0.70 / 2.5 / 8.4 µs per op
(atomics alone at 8 threads: 0.051 µs) — 8 threads give lower total
throughput than one. User-visible at ~50 open segments or 2+ locking workers.
Fix: register (lock word, token, pin) once at slot claim in the isolate's
state; the teardown hook CASes the word away from our token if still held
(`ReleaseHeldLock` already does this check). Expected ~0.04–0.05 µs flat.
Risk: the pin keeps each claimed mutex's ~8 KiB mapping alive for the
isolate's lifetime unless held weakly; a smaller fix is a per-data-pointer pin
cache with a thread-local list.

**P2 — lost wakeups (= Correctness F10).** Path (a): waiter sets
HAS_WAITERS then re-reads `expected`; if the holder unlocked between, it
sleeps on a free word for a full slice and later unlocks never notify. Path
(b): a woken waiter acquires with a bare `token`, erasing HAS_WAITERS. 3
processes, 20 µs critical section, 50 µs think time: original 14.2k acq/s, max
750 ms (11 of 14 slice timeouts were waits on a free word); fix (a) 32k; fix
(a)+(b) 37–41k, zero slice timeouts. 8 processes: 14.1k → 41.4k acq/s, max
752 ms → 5–8 ms, roughly even share. Fix: retry when `(expected & TOKEN_MASK)
=== 0`; acquire with `token | HAS_WAITERS` after waiting (Drepper "mutex2").
Cost ≤ one extra FUTEX_WAKE (~0.16 µs) per contended handoff; low risk.

**P3 — ring wakes per message.** ~0.16 µs per message per side against a
220–450 ns budget. Variant tested with a parked flag in padding words (word 1 =
consumer parked on HEAD, word 17 = producer parked on TAIL; set flag,
re-check, wait, clear; other side notifies only if set):

| Path | Original | With flag |
|---|---|---|
| Zero-copy 8 B | 2.6M | 3.6–4.6M |
| Zero-copy 64 B | 2.5M | 4.1–4.5M |
| Zero-copy 4 KiB | 2.3M | 3.2–3.4M |
| Copying 64 B | 2.1–2.2M | 2.7M |
| Copying 4 KiB | — | within noise |

Low risk (JS Atomics are SC, so flag-then-recheck is safe; a crashed waiter
leaves the flag set, costing only extra wakes). It uses padding words, so the
layout/`layoutVersion` should be decided before release.

**P4 — 250 ms floor on every timeout.** Wait loops pass `sleepSliceMs`
instead of `min(250, deadline - now)`. `lock`/`lockAsync` with 1, 10 or 50 ms
throw E_TIMEOUT after ~250 ms; a short-timeout polling consumer is 250× slower
than asked. Fix: clamp; no risk.

**P5 — non-Linux wait paths.** `SyncWait`'s `#else` polls on macOS/Windows
even where os_sync or named semaphores exist, serving every blocking
`Mutex.lock`, ring `reserve`/`peek` and init join — contradicting
docs/compat.md:16 and PLAN §6. Linux simulation of the backoff: +1.6 ms mean
wake latency (p99 3.3 ms) vs ~4.5 µs futex; CPU 0.12% core per parked waiter,
1.1% for 16, 6.5% for 64. **macOS bug (code reading only, U1 unverified):**
`WaitThreadMain` calls `os_sync_wait_on_address` with no timeout
(cc/wait.cc:284; `sliceMs` computed but unused), so a timed `waitAsync` never
times out without a wake — including `lockAsync`'s 250 ms dead-owner check,
so a dead holder is never stolen via `lockAsync`. **Windows:** every notify
(every ring message) does a registry scan plus `OpenSemaphoreW`/`CloseHandle`
for semaphores sync waiters never create (µs-scale, estimated). Fix: route
`SyncWait` through `os_sync_wait_on_address_with_timeout` / the word
semaphore; with-timeout os_sync in `WaitThreadMain`; cache semaphore handles.
Medium risk — validate on CI hardware.

**P6 — async cap and mux cost.** `g_pending`/`g_threads` are process-wide
across all isolates and segments: 127 muxed + 64 one-thread waits = 191.
Measured: 250 concurrent `lockAsync` on one held mutex → 191 × E_TIMEOUT,
59 × E_TOO_MANY_WAITERS. Each registration bumps the control word and the mux
rebuilds all n entries (shared_ptr copies + kernel re-queueing); each delivery
triggers another rebuild: round trip 7.7 / 12.1 / 21.2 / 39.6 µs with 0 / 32 /
64 / 126 parked; overflow-thread path 41.6 µs. Idle CPU negligible (0.24% core
with 126 waits re-arming every 250 ms). An aborted `lockAsync` holds its slot
until its slice expires. Cheapest mitigation: merge waits on the same
(address, expected) into one `futex_waitv` entry and fan the result out — the
common "many `lockAsync` on one Mutex" pattern then uses one entry. Next:
several mux threads of ≤127 each instead of 64 one-shot threads. Medium risk.

**P7 — liveness checks.** `CheckLiveness` in C: readlink
/proc/self/ns/pid 0.57 µs + `kill` 0.12 µs + fopen/fread /proc/pid/stat
2.1 µs; via JS 3.7 µs. Runs on every pass where the lock is held by someone
else, including right after a real wake; PLAN §7.3 only requires it after a
timed-out slice. 0.96–1.54 checks per contended acquisition with 8 processes;
lazy checking cut that to 0.35–0.72 with unchanged throughput (saving is waiter
CPU). `SelfIdentity` (4.6 µs) re-reads two per-process constants on every claim
and full `open()`. Fix: cache own startTime and pid-ns inode (re-read when
`getpid()` changes), skip the check right after an `'ok'` wake. Low risk.

**P8 — slot claim.** The two passes in `MutexClaimSlot` are identical and each
liveness-checks every ACTIVE slot passed: 5.1 / 55.9 / 133 / 197 µs with 0 / 16
/ 40 / 63 live slots ahead. Bites a 40-worker pool calling `Mutex.open` per
request. Fix: take free slots in pass 1 with no checks; reclaim only in pass 2.

**P9 — stat() empty rows.** `readHeader` 4.1 µs with `maxAttach` 0 vs
98.7 µs with 128 (the 64 KiB pread + open/close is within the 4 µs). Fix: skip
rows with refcount ≤ 0 natively. Only matters if `stat()`/`reap()` are polled.

**P10 — initState never woken.** Joiners wait at header.cc:232 but nothing
calls `SyncWake` on that word; reachable during a concurrent grow, up to
250 ms. Fix: wake all after each write of ready.

**P11 — bench fidelity.** The pong process spins at 100% CPU and never parks;
a true round trip is ~4.5 µs. Reword docs/benchmarks.md or add a
park-on-both-sides row.

**Correctness note (code reading):** the creator never moves initState 0→1
(header.cc:4 says it does; header.cc:146-155 doesn't), so a joiner arriving
during the creator's ftruncate/fallocate takes over initialization — see
Correctness F5.

### 5.4 Cleared (measured)

- `sync.notify`: one crossing + one FUTEX_WAKE, 0.157 µs vs 0.120 µs raw;
  `sync.wait` not-equal path 0.344 µs, one syscall.
- Real sleep/wake ping-pong 4.5 µs — the kernel's cost.
- `open()` reusing a cached mapping 1.67 µs, no syscalls.
- Ring per-message allocations: `subarray` 29 ns, pending object 37 ns,
  `read()` copy 73 ns; 4 KiB/64 KiB copying paths copy-bound (~2 GB/s).
- Idle cost of the 250 ms slice on Linux: 0.24% core with 126 parked async
  waits; ~4 wakes/s per parked sync waiter.
- wait.cc lock ordering: `g_mu` → `hub->mu` only in `Deliver`; no inversion.
- Startup: `require` 2.6 ms; first native load 1.3 ms once per process;
  `EnsureMux` probe once.
- `list()`: 1.1 ms over 502 /dev/shm entries.

### 5.5 Pre-release recommendation

P1, P2, P3, P4; P5 (at minimum the macOS timeout and routing `SyncWait`
through the documented mechanisms, since compat.md describes behaviour the
code doesn't have); P7; the init race. P6 (same-word merging) if `lockAsync`
is pitched for servers, otherwise document the per-process 191 cap. P8–P11 can
follow the release.

---

## 6. Review 3 — Security

### 6.1 Verdict

The documented trust model concedes that a same-uid process with /dev/shm
access can read/corrupt a segment it opens. The effective boundary today is
weaker in one decisive way: a segment's **header is trusted to describe the
mapping's own geometry**, but it is attacker-writable and never validated
against the real mapped size. Merely *joining* a segment someone else created
— the normal operation — can hand JavaScript an SAB whose `byteLength` exceeds
the mapped memory, yielding cross-process OOB reads and writes of unrelated
process memory and remotely triggerable SIGSEGV/SIGBUS/SIGABRT. This is
reachable by the weakest adversary in scope (anyone who can create a file in
world-writable /dev/shm, a compromised child, or an in-process library) and
goes beyond the documented model, which only concedes corruption of the
segment's own bytes. **Not safe to publish as-is.** Name namespace,
mutex/ring control-word integer handling and Windows percent-escaping are
sound; the symlink attack is blocked by glibc's `O_NOFOLLOW`.

### 6.2 Findings

| ID | Sev | Title | Adversary | Location |
|---|---|---|---|---|
| F1 | P0 | Size-less join trusts `header.dataBytes` for SAB length → OOB SAB over unmapped/foreign memory | /dev/shm file creator; compromised child; in-proc lib | cc/addon.cc:380-421 (`Open`), cc/header.cc InitOrJoin ready branch |
| F2 | P0 | Sized join (exact/at-least/grow) never checks existing `st_size` → SIGBUS, and OOB combined with F1 | same | cc/segment.cc:206-219, cc/addon.cc:376-397 |
| F3 | P0 | Takeover indexes `attachTable[initializerSlot]` with attacker-controlled `headerBytes`/`initializerSlot` → OOB read / SIGSEGV | same | cc/header.cc:193-198 |
| F4 | P0 | RingConsumer/RingProducer inherit F1 → `read()` returns foreign memory; `reserve()` hands a write window into it | same | src/ringbuffer.ts:117-132, 257-271 |
| F5 | P1 | Benign grow-vs-join and create/unlink/recreate races produce oversized SABs and SIGBUS with no attacker | none | cc/addon.cc full-open path; cc/segment.cc join sizing |
| F6 | P1 | Registry split-brain: cached Mapping survives another process's unlink+recreate | none / same-uid | cc/registry.cc:88 (`Registry::Find`), cc/addon.cc:334-341 |
| F7 | P2 | Liveness returns "alive" with unverified start time when `/proc/<pid>/stat` is unreadable (hidepid) or owner is a zombie | same-uid / environment | cc/liveness.cc:104-110 |
| F8 | P2/P3 | Windows object namespace is case-insensitive; names differing only in case share one section (compile-only) | same-session | cc/segment.cc:78-99 |

### 6.3 Details

**F1 — OOB SAB via hostile header (P0, confirmed).** Boundary: a joiner treats
creator-written header fields as describing its own mapping size. Trace
(size-less join, the `RingConsumer.open` / `open(name)` path):

1. `Open` (cc/addon.cc): `haveSize=false` → `opts.mode = Mode::kJoin`
   (line 355).
2. `OpenSegment` (cc/segment.cc): `wholeObject` true; `shm_open(O_RDWR)`;
   `total = max(fileSize, headerBytes)` (line 208) — the real file size;
   `mmap(total)`. `mappingBytes` is correct.
3. `InitOrJoin` ready branch (cc/header.cc): validates `magic` and
   `headerBytes == EffectiveHeaderBytes()` but never reads or validates
   `dataBytes`.
4. Back in `Open` (cc/addon.cc:389-396): `windowBytes =
   HeaderDataBytes(header)` — attacker-controlled.
5. `MakeWindow` (cc/addon.cc:214-223): `NewBackingStore(base + headerBytes,
   windowBytes)` with no clamp to `mappingBytes - headerBytes`.

`TryReuse` (cc/addon.cc:253-276) is not the hole — it compares
`want > m->dataBytes` and falls back to a full open; the full-open path has no
equivalent check.

PoC: attacker creates an 8192-byte file (4 KiB header + 4 KiB data) with
`dataBytes = 1 MiB`; victim `open(name)`:

```
victim SAB.byteLength = 1048576   actual mapping bytes = 8192
offset of next VMA inside victim SAB = 4096 (within byteLength: true)
next VMA: anon_inode:[io_uring]  (libuv's ring, rw-s)
OOB READ of foreign memory via SAB: 00000000...ff000000ff01000000010000000200000000...
```

With `dataBytes = 2^40`: reading or writing `sab[2^39]` → SIGSEGV. With
`dataBytes` = u64 max or > 2^53: V8 fatal `Check failed: byte_length <=
kMaxByteLength` → SIGTRAP. Reading/writing libuv's io_uring ring adjacent to
the mapping is a plausible path to further corruption, not just a crash.

Fix: make the mapping the single source of truth for geometry. After
`OpenSegment`, compute `maxWindow = mappingBytes - headerBytes` and
clamp/validate every `windowBytes`/`mappedDataBytes` against it; on a ready
join, reject (`E_INCOMPATIBLE`) when `header.dataBytes > maxWindow`. Header
`dataBytes` is a hint bounded by `st_size`, never an authoritative
`NewBackingStore` length.

**F2 — sized join never fstat-checks the existing object (P0).** On `EEXIST`
the create attempt falls to `shm_open(O_RDWR)` (cc/segment.cc:160-173). For a
non-raw, non-wholeObject join the only size handling is
`if (!created && fileSize < headerBytes) ftruncate` (lines 211-218): it fixes a
file smaller than the header page but never checks
`fileSize >= headerBytes + requested`. `total = requestedTotal` is mapped
regardless; touching past EOF is SIGBUS. PoC (4096-byte attacker file, header
`dataBytes` matching the request):

```
[exact, short file]     SIGBUS   (victim: open(name,65536))
[at-least, short file]  SIGBUS   (PLAN §5.3 claims at-least "can never SIGBUS")
[grow, short file]      SIGBUS
[Mutex.open short file] SIGBUS
[size-less, 0-byte obj] SIGBUS   (also models a crashed creator)
```

The at-least case contradicts PLAN §5.3. The 0-byte case needs no attacker —
a creator that died between `shm_open(O_CREAT)` and `ftruncate`. Fix: on every
join, fstat and require `st_size >= headerBytes + requested` (sized) or map
only `min(st_size, …)` (prefix/at-least), else `E_INCOMPATIBLE` /
`E_SIZE_MISMATCH`. Only the creator may `ftruncate` up.

**F3 — takeover OOB via attacker `headerBytes`/`initializerSlot` (P0).** In
InitOrJoin's non-ready branch (cc/header.cc:193-198):

```
int initSlot = h->initializerSlot;                    // attacker: 0x1000000
const uint32_t n = AttachSlotCount(h->headerBytes);   // attacker headerBytes=2GiB -> n huge
if ((uint32_t)initSlot >= n || refcount(attachTable[initSlot]) <= 0) ...  // passes
```

`AttachSlotCount` guards only underflow (`headerBytes < sizeof(Header)`), not
an upper bound against `mappingBytes`, so `&h->attachTable[initSlot]` is far
outside the 8 KiB mapping. PoC: `initState=1`, `headerBytes=0x80000000`,
`initializerSlot=0x1000000` → victim `open(name, 4096)` → SIGSEGV. Fix:
validate `headerBytes` against the real mapping (reject if `> mappingBytes` or
`!= EffectiveHeaderBytes()`) before computing `AttachSlotCount` or indexing,
on every pre-ready branch. Same root cause as F1.

**F4 — RingBuffer consequences of F1 (P0).** `RingConsumer.open` /
`RingProducer.open` always join size-less and derive
`capacity = sab.byteLength - RING_HEADER_BYTES`, so capacity is
attacker-sized. Consumer: with `dataBytes` = 1 GiB + 256 and `head`/`tail`
steered so `pos` lands on the adjoining io_uring VMA, `c.read()` returned 255
bytes of foreign memory. Producer: `p.reserve(64)` returned a view whose
`byteOffset` lands in the foreign VMA; after `fill(0x41)` + `commit()` the
process continued briefly, then SIGABRT — a write primitive into neighbouring
memory. Within a correctly sized SAB, ring arithmetic is masked and `subarray`
clamps; a hostile `lenWord = 0x7fffffff` only makes the consumer wait. Fixing
F1/F2 closes this.

**F5 — benign races (P1, no attacker).** Grow vs size-less join: one process
repeatedly grows, another repeatedly joins size-less; the joiner saw 5,474 of
168,257 joins with an SAB exceeding any mapping it held (checked via
`/proc/self/maps` without touching bad pages). Create/unlink/recreate vs
size-less join: a creator looping create→unlink and three joiners (GC forced
to defeat registry reuse) — all three joiners died of SIGBUS. Same
geometry-trust bug; fixed by F1/F2's clamp. These will appear as flaky
production crashes.

**F6 — registry split-brain (P1).** `Registry` keys by name; another
process's `unlink` doesn't invalidate the cached `Mapping`. PoC: A opens and
keeps the SAB; another process unlinks, re-creates and writes byte0=42; A
re-opens and `TryReuse` returns the stale mapping:

```
client sees byte0 = 0   (service wrote 42 — not visible)
service sees byte1 = 0   (client wrote 7 — not visible)
```

PLAN §9's attach-table design handles only the first-exit-unlink variant.
Fix: on reuse, compare `fstat(m->fd).st_dev/st_ino` with a fresh stat of the
name; drop the entry on mismatch.

**F7 — liveness with unverified start time (P2).** cc/liveness.cc:104-110:
`kill(pid,0)==0 || errno!=ESRCH` → candidate alive, then
`if (ReadStartTime(...) && start != id.startTime) return kDead`. When
`/proc/<pid>/stat` is unreadable (hidepid) `ReadStartTime` fails, the
start-time check is skipped and the result is `kAlive` with the pid-reuse
guard disabled; same for a zombie. Fail-safe for correctness (never a wrongful
steal) but an availability gap: a dead owner whose pid was reused is treated
as alive forever, so the mutex/role is never stolen and `reap` never collects
it. Fix: return `kUnknown` when start time is unverifiable and document that
recovery then needs `reap`/manual action; at minimum document hidepid/zombie
behaviour in compat.md.

**F8 — Windows name case collision (P2/P3, compile-only).** `ObjectName`
(cc/segment.cc:78-99) percent-escaping is injective (`/`→`%2F`, `%`→`%25`,
one leading `/` only), but NT object names under `Local\`/`Global\` are
case-insensitive, so `/ABC` and `/abc` share one section. Fix: document, or
encode to a case-preserving object name.

### 6.4 Cleared

- **Symlink on the join path:** glibc `shm_open` uses `O_NOFOLLOW`; a planted
  symlink fails with ELOOP (`E_SYSTEM`) and the attacker file was not written
  through. Create path `O_CREAT|O_EXCL|O_NOFOLLOW` is safe. (Worth a
  probe/CI note for non-glibc libcs.)
- **Mutex native indexing from a hostile token:** `MutexOwnerAlive` checks
  `slot >= slotCount` → "alive" before any slot access (cc/mutex.cc:129-135);
  claim loops bounded by `kMutexSlotCount`.
- **Ring control-word arithmetic within a correctly sized SAB:** `pos` masked,
  huge `lenWord` only makes the consumer wait (src/ringbuffer.ts:311),
  `subarray` clamps. (Note: Correctness F1 shows that same guard deadlocks at
  the 2^31 wrap.)
- **`stat`/`readHeader` pread loop:** fixed 64 KiB buffer, `headerBytes`
  clamped to `sizeof(buf)`, rows to `maxAttach` (cc/segment.cc:323-350);
  attacker rows only affect displayed data and `reap`, which fails safe.
  (Correctness F29 adds that a short file leaves rows uninitialized.)
- **`AttachSlotCount` underflow:** guarded; only the upper bound is missing
  (F3).
- **Windows percent-escaping:** injective (see F8).
- **`WordAddrOf`/`DataAddrOf`:** validates `IsInt32Array` and index range
  before `base + ByteOffset/4 + index` (cc/addon.cc:530-542).
- **Size validation:** `1 ≤ size ≤ 4 GiB` and `SHM_BRIDGE_MAX_SEGMENT_BYTES`
  apply to requested sizes only, not header-declared `dataBytes` (that gap is
  F1).

### 6.5 Publish-blocking order

1. F1 + F3 together — validate all header geometry (`headerBytes`,
   `dataBytes`, `initializerSlot`) against the real mapping before sizing a
   BackingStore or indexing the attach table.
2. F2 — fstat-check existing object size on every join; only the creator
   ftruncates. Fixing F1/F2 also closes F4 and F5.
3. F6 — validate cached mapping identity on reuse.
4. F7, F8 — document and/or harden; may ship with documented caveats in
   compat.md.

Do not publish until items 1–3 are fixed.
