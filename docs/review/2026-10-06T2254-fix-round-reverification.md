# Fix-round re-verification — 2026-10-06 22:54 (+05:30)

| | |
|---|---|
| Commit verified | `0f01571` (fix round A–F on top of `e401d1c`), clean working tree |
| Original review | [2026-10-06T1909-pre-release-review.md](./2026-10-06T1909-pre-release-review.md) |
| Fix commits | `922dcfb` A (geometry + init protocol) · `b94d91b` B (registry identity) · `2446684` C (P0 hangs) · `fe1cdf8` D (mutex wake, ring flags, slices, async ring) · `a5a153e` E (macOS/Windows) · `0f01571` F (timeouts, stat liveness, parity) |
| Platform of evidence | Linux 7.2.9 (Fedora 43), x86_64, 18 cores, Node 24.18 |
| Build | `npx node-gyp rebuild` clean (2 warnings: upstream `node.h` cast; unused `HubFor` at `cc/wait.cc:115`) |
| Suite | `npm test`: 81 tests, 80 pass, 1 env-gated skip |
| Mode | Read-only. Three re-verification passes (correctness, performance, security) plus independent repros. All segments used `/shm-bridge-test-rv*-` / `/shm-bridge-sec-rv-` names; `/dev/shm` clean afterwards. |

macOS and Windows statements are from source reading only. The security
pass was partly interrupted (exploit-style PoCs that write past a mapping
were not built or run); its S1/S2 are confirmed by source reading, and its
skipped F2/F5 checks were run independently (§2).

---

## 1. Summary

**The original P0s are closed on Linux.** The ring 2^31 wrap, leaked mutex
slots, hub reuse by later workers, and zombie holders are all fixed and
reproduced as fixed. The original cross-process OOB class (hostile
`dataBytes` / short file / takeover indexing) now fails safely with
`E_INCOMPATIBLE`, and a 15 s race soak produced zero oversized SABs and zero
SIGBUS. The performance targets were hit: mutex lock+unlock ~3.3× faster and
flat across threads and mappings, small ring messages 2–3.5× faster,
timeouts honour their value.

**Not yet publishable.** The fix round introduced regressions, mostly from
one design choice in commit A — joiners map `min(requested, st_size)` once
and never re-map — plus two places where header geometry is re-read from
shared memory after validation:

- Cross-process prefix and smaller-size opens now fail (`at-least` is
  broken across processes).
- Ordinary concurrent create/join fails spuriously (~0.5–5%).
- A losing `grow` silently returns a smaller SAB than requested.
- Two TOCTOU paths can still push native code past the mapping.
- `reserveAsync`/`peekAsync` lose every wakeup (250 ms per wait).
- Worker teardown can SIGSEGV; claimed mutexes leak a mapping + fd each.
- Windows joins always fail; macOS still does not compile.

### Publish blockers (recommended order)

1. **Geometry, read once and re-map** (R1–R5): snapshot the header into
   locals once and validate; after reaching ready, re-fstat and re-map so
   the mapping covers the validated `dataBytes`; throw when the window is
   smaller than `requested`; never re-read `headerBytes`/`dataBytes` from
   shared memory afterwards; length-check `DataAddrOf`.
2. **Mutex lifetime** (R11, R12): take the pin from the SAB's BackingStore,
   release it when the Mutex is collected or closed; never walk unmapped
   `claimedData` at teardown.
3. **Async ring wakeups** (R14): set/re-check/clear parked flags in
   `reserveAsync`/`peekAsync`.
4. **Init protocol** (R6–R9): size-less joins never initialize; creator CAS
   0→1 with the initializer published atomically; creator failure unlinks
   and restores; grow-only truncation in the grace path.
5. **Platforms** (R16, R17): fix Windows join size and the macOS `os_sync`
   declarations/symbols — or mark both unsupported in `docs/compat.md` and
   keep them out of the release claims until CI passes.
6. **Remaining original items**: mutex lost wakeup residual (R13), F13, F14
   (unlink-before-first-lock order), F20, ring producer token 0 (F12),
   macOS liveness (F4/F7).

### Counts

| Pass | Original findings | Fixed | Partial | Not fixed | Documented only | Regressed | New |
|---|---|---|---|---|---|---|---|
| Correctness (F1–F38) | 38 | 16 | 17 | 4 | 0 | 1 | 16 |
| Performance (P1–P11) | 11 | 7 | 2 | 1 | 1 | 0 | 8 |
| Security (F1–F8) | 8 | 5 | 1 | 2 | 0 | 0 | 5 |

The new findings overlap heavily across passes; §5 merges them into 24
distinct items (R1–R24).

---

## 2. Independent checks

Run or read while assembling this document, at `0f01571`:

| Check | Result |
|---|---|
| Sec F2 — short file under exact / at-least / grow / `Mutex.open` / size-less (fresh child process each) | All throw `E_INCOMPATIBLE`; no SIGBUS. **Fixed.** |
| Sec F5 — 15 s soak: 1 grower + 2 size-less joiners comparing SAB vs `/proc/self/maps`; 1 create/unlink/recreate loop + 3 joiners writing the last byte | 236,164 joins, **0 oversized SABs**; 6,427 touching joins, **0 SIGBUS**. Failures were clean `E_NOT_FOUND`/`E_INCOMPATIBLE`. **Fixed.** 285 legitimate joins (~0.1%) got a spurious `E_INCOMPATIBLE` during concurrent grow → R2. |
| Size-less join of a 0-byte object | Waits ~280 ms, takes over, publishes a **ready 0-byte segment**; later `open(name, 65536)` → `E_SIZE_MISMATCH` until unlinked → R6. |
| Cross-process `open(name, 4096, {sizePolicy})` on an existing 8192-byte segment | `at-least`, `grow`, `exact` all → `E_INCOMPATIBLE` (before the fixes: OK, OK, `E_SIZE_MISMATCH`) → R1. |
| `reserveAsync`/`peekAsync` parked flags | Flags stored only in sync paths (`src/ringbuffer.ts:231`, `:373`, `:397`); none in `reserveAsync` (`:281`) or `peekAsync` (`:435`) → R14. |
| Sec S1 — attach-row scans | `ClaimAttachRow`/`EnsureAttachedRow`/`ReleaseDeadRows` call `AttachSlotCount(h->headerBytes)` (`cc/header.cc:31,46,52,75`) after the ready branch validated it at `:210` → R4. |
| Sec S2 — `dataBytes` re-read | Validated in `InitOrJoin` (`cc/header.cc:219-224`), re-read at `cc/addon.cc:414` (policy) and `:431` (window); `DataAddrOf` (`cc/addon.cc:680-686`) has no length check → R5. |
| Sec S3 — grow floor | `finalBytes = max(target, HeaderDataBytesOf(h))` (`cc/header.cc:357-358`). The ready branch rejects a static hostile value first, so this needs a race; a same-uid attacker can inflate the file directly anyway → rated P2 (security pass said P1). |

---

## 3. Status of original findings

### 3.1 Correctness (F1–F38)

| ID | Sev | Status | Evidence |
|---|---|---|---|
| F1 | P0 | FIXED (verified) | `src/ringbuffer.ts:394` unsigned; delivers across 2^31 (2^32 untested) |
| F2 | P0 | FIXED (verified) | `cc/mutex.cc:301-303`; 80 sequential workers then main locks |
| F3 | P0 | FIXED (verified) | `cc/wait.cc:740-760`; 12 sequential workers all settle |
| F4 | P0 | PARTIAL | Linux fixed (`cc/liveness.cc:135`, zombie stolen in 0.1 ms); macOS `CheckLiveness` (`:157-168`) has no zombie check |
| F5 | P1 | PARTIAL | Live creator protected (`cc/header.cc:169-174`); 6 sizes × 40 real races, no split — but R7, R8 |
| F6 | P1 | FIXED (verified) | Zero-length object → clean error, no SIGBUS; leaves R6 behind |
| F7 | P1 | FIXED (verified) | `cc/header.cc:219-224`, `cc/addon.cc:434`; real race now yields false `E_INCOMPATIBLE` (R2) |
| F8 | P1 | PARTIAL | No SIGBUS, header never shrinks; 51/90 concurrent growers got `E_INCOMPATIBLE` (R2), one 1 MiB grow returned 4096 bytes (R3) |
| F9 | P1 | FIXED (verified) | `src/sync.ts:54-67`; 1e19/1e300 rejected; untimed wait beside a 2^31 wait resolves normally |
| F10 | P1 | PARTIAL | 3-waiter case fixed; parks on a no-bit word remain (R13) |
| F11 | P1 | FIXED (verified) | `src/mutex.ts:247`; caller catches `E_TOO_MANY_WAITERS`, process exits 0 |
| F12 | P1 | PARTIAL | Mutex skips token 0 (`cc/mutex.cc:82`); ring producer role still gets token 0 (`cc/mutex.cc:367-387`) |
| F13 | P1 | NOT FIXED | Reclaim still CASes `state` (`cc/mutex.cc:153`); stale-gen token still "alive" (`:171`) |
| F14 | P1 | PARTIAL | Fixed when claimed before unlink; unlink-then-first-lock still deadlocks (`cc/mutex.cc:232`, `:284`); see R11 |
| F15 | P1 | PARTIAL | Creator side closed (row claim `cc/header.cc:172` before magic `:178`); joiner side documented in PLAN §9; `Detach` race neither fixed nor documented (`cc/registry.cc:59-75`) |
| F16 | P1 | FIXED (verified) | `cc/addon.cc:339-342`; unlink without recreate falls through correctly; no fd leak |
| F17 | P1 | PARTIAL | `ResolveOnLoop` resets context; promises queued on the hub at teardown are never drained (`cc/wait.cc:703-760`) |
| F18 | P1 | PARTIAL | Thread path sends under `hub->mu` (`cc/wait.cc:396`); `Deliver` still sends after unlocking (`:205`) despite the comment at `:193` |
| F19 | P1 | NOT FIXED | `os_sync_wait_on_address_with_timeout` never declared (`cc/wait.cc:326,349,534-545`), wrong arity; see R17 |
| F20 | P2 | NOT FIXED (verified) | `OthersShareBase` compares `m->base` (`cc/registry.cc:150`), which differs per mmap; grown SAB still unlinked under it |
| F21 | P2 | FIXED (reading) | `cc/registry.cc:132` identity check before `shm_unlink` |
| F22 | P2 | FIXED (test) | `src/mutex.ts:141-152`; PLAN §7.3 text still says the steal sets `ownerDied = 1` |
| F23 | P2 | PARTIAL | Stuck RESERVED now `E_TIMEOUT` in 0.29 ms (no spin) but never recovers; same-thread re-claim works; no `close()` |
| F24 | P2 | FIXED (reading) | readHeader default 2048 rows |
| F25 | P2 | PARTIAL | Mutex claim refuses `startTime = -1` (`cc/mutex.cc:110`); failure cached per thread (R19); role claims unchecked; macOS no guard |
| F26 | P2 | FIXED (reading) | Grower records itself (`cc/header.cc:339-343`); small window remains (R8) |
| F27 | P2 | REGRESSED | Join no longer creates and UNLINKED is checked (`cc/header.cc:207`), but every Windows join now fails (R16) |
| F28 | P2 | FIXED (reading) | Windows wakes ≤1024 permits in 64-permit chunks |
| F29 | P2 | FIXED (reading) | Rows bounded by bytes read (`cc/segment.cc:410-413`) |
| F30 | P2 | PARTIAL | `reserveAsync`/`peekAsync` exist, joiner validation works; async wakes lost (R14); `RingConsumer` does not cross-check the capacity word (`src/ringbuffer.ts:332-346`) despite commit D |
| F31 | P3 | NOT FIXED | `cc/segment.cc:236-241` unchanged, undocumented |
| F32 | P3 | FIXED (reading) | `cc/wait.cc:459` skips delivered/cancelled nodes |
| F33 | P3 | PARTIAL | Cross-process kind mismatch → `E_INCOMPATIBLE`; same-process registry reuse ignores kind |
| F34 | P3 | PARTIAL | Linux native liveness in `stat()`; Windows hard-coded `'unknown'` (`src/ops.ts:103-104`), never reaps |
| F35 | P3 | PARTIAL | `initTimeoutMs` validated, monotonic deadlines; `lock({timeoutMs: NaN})` hangs forever (R20) |
| F36 | P3 | PARTIAL | Fallback join → `E_NOT_FOUND`; per-isolate map and Mutex/Ring opt-in unchanged |
| F37 | P3 | PARTIAL | `'not-equal'` verified; slot-exhaustion `E_TIMEOUT` claimed documented but no doc found; unpruned `g_pending.size()` still counted |
| F38 | P3 | FIXED (reading) | Error-ctor hook registered once per isolate |

### 3.2 Performance (P1–P11)

"Before" was re-measured in the same session from a scratch build of
`e401d1c`; original-report numbers in brackets.

| ID | Status | Before | After | Method |
|---|---|---|---|---|
| P1 | FIXED (measured) | 0.16 µs [0.188]; 100/500 mappings 1.55/6.96 µs; 1/2/4/8 threads 0.17/0.70/2.5/8.4 µs | 0.048–0.056 µs; with mappings 0.048–0.061; 1/2/4/8 threads 0.057–0.065 / 0.065–0.068 / 0.071–0.082 / 0.095–0.109 µs | 100k lock+unlock ×5 |
| P2 | PARTIAL | 3 proc 13.7k acq/s, max 502–752 ms; 8 proc 13.3–14.1k, max 1.0–1.75 s | 3 proc 40.0–42.2k, max 22–92 ms; 8 proc 38.7–41.6k, max 3–9 ms, 0 slice timeouts — **tighter loops: 8–32 slice timeouts, max 251–502 ms** (R13) | N procs, 20 µs CS, 50 µs think |
| P3 | FIXED (measured) | zero-copy 8/64 B 2.84M/2.48M; copy 64 B 2.35M; 1 wake/msg/side | zero-copy 8/64 B 8.4M/8.6M; 4 KiB 4.7M; copy 64 B 5.2M; 0–0.009 wakes/msg | cross-process, 1 MiB ring |
| P4 | FIXED (measured) | 250.1–250.6 ms for 1/10/50 ms | 1 → 1.03–1.37, 10 → 10.03–10.69, 50 → 50.08–50.66 ms (all six APIs) | holder in child |
| P5 | NOT FIXED (reading) | — | macOS does not compile; wake symbol wrong (R17); Windows matches docs | code reading |
| P6 | DOCUMENTED ONLY | 10.2–11.5 / 12.9 / 21.1–21.8 / 41.9–42.6 µs | 10.2–10.7 / 12.7–14.5 / 21.2–22.4 / 40.5–45.5 µs at 0/32/64/126 parked; cap 191 per process (PLAN §6, `docs/compat.md`) | waitAsync ping-pong |
| P7 | PARTIAL | selfIdentity 4.4 µs; check 3.46 µs | selfIdentity 0.74 µs; check still 3.4 µs (readlink of own ns per call, `cc/liveness.cc:116`), still on every contended pass (0.72–1.03 per acquisition) | direct calls |
| P8 | FIXED (measured) | 50/119/218/317 µs at K=0/16/40/59 | 6.6–9.8 µs flat at K=0/16/40/62 | fresh worker claim |
| P9 | FIXED (measured) | readHeader 113 µs; stat 126 µs; reap(500) 63 ms | 4.0 µs; 10 µs; 6.4 ms; `list()` 1.6 ms | 500 live segments |
| P10 | FIXED (reading) | — | SyncWake after initState writes (`cc/header.cc:174-175,183-184,265-266,298-300,364-365`); gaps at `:349` (grow fstat error) and takeover CAS `:248` | code reading |
| P11 | FIXED (measured) | pong spun a core | pong parks; round trip 5.3–5.6 µs | bench ×3 |

**No regression** in: HAS_WAITERS stickiness (one extra wake after a burst,
then 0.049 µs/op), unblocked ring path (+1 atomic load, net 3× faster),
worker lifecycle with a Mutex (22.2–22.8 ms vs 23.5 ms; 0 leaked slots after
30 exits vs 30), sync ring stream 2 × 20 s (max 30–32 ms, 0 sequence errors).

### 3.3 Security (F1–F8)

| ID | Sev | Status | Result now |
|---|---|---|---|
| F1 | P0 | FIXED (static); race residual R5 | Hostile `dataBytes` → `E_INCOMPATIBLE`; window clamp `cc/addon.cc:427-434` |
| F2 | P0 | FIXED (verified, §2) | All short-file modes → `E_INCOMPATIBLE` |
| F3 | P0 | FIXED at original site; same class as race R4 | Row count bounded by mapping (`cc/header.cc:276-277`) |
| F4 | P0 | FIXED | Ring capacity validated before role claim (`src/ringbuffer.ts:136-157`, `:335-342`) |
| F5 | P1 | FIXED (verified, §2) | 0 oversized SABs, 0 SIGBUS in 15 s soak |
| F6 | P1 | FIXED | dev/ino check (`cc/registry.cc:94-110`, `cc/addon.cc:354`) |
| F7 | P2 | PARTIAL | Linux → Unknown when stat unreadable (`cc/liveness.cc:75`, PLAN §7.1); macOS still alive on `proc_pidinfo` failure |
| F8 | P2/P3 | NOT FIXED | No case-sensitivity handling or note anywhere |

Cleared list re-confirmed: symlink blocked (glibc `O_NOFOLLOW`); malformed
mutex token never indexes (`cc/mutex.cc:169`); claim loops bounded (64
slots; ring claim 10,000 iterations then `E_TIMEOUT`, `cc/mutex.cc:317,398`);
ring arithmetic masked and unsigned (`src/ringbuffer.ts:394`); ReadHeader
bounded by bytes read, `maxAttach` and 64 KiB; `WordAddrOf` range-checked
(`cc/addon.cc:569-581`); forged attach rows can block `reap()` but cannot
make it unlink a live segment (fail-safe). A same-uid attacker shrinking the
file after a victim's fstat+mmap remains ordinary POSIX corruption inside
the documented model.

---

## 4. Cross-reference of new findings

| R | Correctness | Performance | Security | Independent |
|---|---|---|---|---|
| R1 | N1 | — | — | reproduced |
| R2 | N2 | N2 | — | soak (~0.1%) |
| R3 | N5 | — | — | — |
| R4 | — | — | S1 | source |
| R5 | — | — | S2 | source |
| R6 | N3 | — | — | reproduced |
| R7 | N4 | N2 (note) | S4 | — |
| R8 | N4 | N8 | S4 | — |
| R9 | N7 | N8 | — | — |
| R10 | — | — | S3 | source |
| R11 | N8 | — | — | — |
| R12 | N11 | N4 | — | — |
| R13 | N13 | P2 residual | — | — |
| R14 | N6 | N1 | — | source |
| R15 | N16 (part) | N8 | — | — |
| R16 | N12 | — | — | — |
| R17 | F19 | P5 | — | — |
| R18 | — | P7 | — | — |
| R19 | N9 | — | — | — |
| R20 | N10 | — | — | — |
| R21 | N14 | N6, N7 | — | — |
| R22 | — | N3, N5 | — | — |
| R23 | N15 | — | S5 | — |
| R24 | N16 | docs items | — | — |

---

## 5. New findings (merged)

| ID | Sev | Title | Location |
|---|---|---|---|
| R1 | P1 | Cross-process prefix / smaller-size opens fail with `E_INCOMPATIBLE` | `cc/segment.cc:289-293`, `cc/header.cc:219-224` |
| R2 | P1 | Concurrent create/join and grow/join fail spuriously with `E_INCOMPATIBLE` | same |
| R3 | P1 | Losing `grow` silently returns a SAB smaller than requested | `cc/addon.cc:434` + `GrowSegment` 0 return |
| R4 | P0 (race, by reading) | Attach-row scans re-read shared `headerBytes` after validation → CAS/store past the mapping | `cc/header.cc:31,46,52,75`; `cc/registry.cc` Detach scan |
| R5 | P0 (race, by reading) | `dataBytes` re-read after validation + unchecked `DataAddrOf` → native mutex access past SAB and mapping | `cc/addon.cc:414,431,680-686`; `src/mutex.ts:89` |
| R6 | P2 | Size-less join initializes a crashed creator's object as a ready 0-byte segment | `cc/addon.cc:383-384`, `cc/header.cc:248-266` |
| R7 | P2 | Multiple initializers after a dead initializer; creator stores state 1 without CAS | `cc/header.cc:174,253-259,298` |
| R8 | P2 | Grace-path `ftruncate(headerBytes)` from a stale fstat can shrink a sized object; grower window before recording itself; grow error paths leave state 1 | `cc/segment.cc` joiner branch; `cc/header.cc:339-354` |
| R9 | P1 | Creator sizing failure leaves the name stuck in state 1 for the creator's lifetime; on macOS (U2) every non-raw create may fail this way | `cc/header.cc:174-176`; `cc/segment.cc:255` |
| R10 | P2 | Grow trusts header `dataBytes` as a size floor (bypasses `SHM_BRIDGE_MAX_SEGMENT_BYTES` under a race) | `cc/header.cc:357-358` |
| R11 | P1 | SIGSEGV at worker teardown walking unmapped `claimedData` (mutex claimed after unlink, instance GC'd) | `cc/mutex.cc:301-303` |
| R12 | P1 | Claim-time pin keeps every claimed Mutex's mapping + fd for the isolate's lifetime | `cc/mutex.cc:232-233` |
| R13 | P2 | Mutex waiter parks on a re-read word without HAS_WAITERS → 250–500 ms stalls | `src/mutex.ts:158-169` (+ `lockAsync`) |
| R14 | P1 | `reserveAsync`/`peekAsync` never set parked flags → every async wake costs the slice | `src/ringbuffer.ts:281-297,435-444` |
| R15 | P3 | Stale parked flag after a crashed waiter → one FUTEX_WAKE per message forever; same-thread `RingConsumer` re-claim allows two consumers | `src/ringbuffer.ts`; `cc/mutex.cc` role claim |
| R16 | P1 | Windows: `mode:'join'` sets `mappingBytes = 0` → every join (incl. `RingConsumer.open`) fails | `cc/segment.cc:130`, `cc/addon.cc:368-372` |
| R17 | P1 | macOS: `os_sync_wait_on_address_with_timeout` undeclared and wrong arity; `os_sync_wake_by_address_shared` not a real export → no compile, and wakes would be no-ops | `cc/wait.cc:214-219,217,326,349,534-545` |
| R18 | P3 | Liveness still readlinks own pid-ns per check and runs on every contended pass | `cc/liveness.cc:116`; `src/mutex.ts` |
| R19 | P2 | A failed first identity read (EMFILE) is cached per thread forever | `cc/liveness.cc:98-104` |
| R20 | P2 | `lock({timeoutMs: NaN})` waits forever, never re-checks liveness | `src/mutex.ts:37-38,114,169` |
| R21 | P3 | Non-positive timeout → effectively infinite `SyncWait`; commit F's "EINVAL → 1 ms park" is not in the code | `cc/wait.cc:466-474,507-510`; `cc/header.cc:290-291` |
| R22 | P3 | Cost regressions: 2 ms grace poll floor for joiners racing a creator; registry reuse 1.6 → 2.75 µs | `cc/segment.cc` grace loop; `cc/registry.cc` identity check |
| R23 | P3 | Row leak when `Open` throws after `InitOrJoin` claimed a row; `std::min` binds a reference into shared memory | `cc/addon.cc` Open; `cc/header.cc:276` |
| R24 | P3 | Documentation and comment drift | see below |

### Details

**R1 — cross-process prefix opens fail (P1, reproduced).** Joins now map
`min(requested, st_size)`. A second process calling
`open(name, 4096, {sizePolicy:'at-least'})` on an existing 8192-byte segment
maps 4096 bytes, then the ready-branch check sees `dataBytes` 8192 > its
4096-byte window and throws `E_INCOMPATIBLE`. `grow` with a size not larger
than current and `exact` with a smaller size fail the same way (the latter
should be `E_SIZE_MISMATCH`). PLAN §5.3 prefix semantics are broken across
processes; in-process tests pass only because registry reuse answers. **Fix:**
validate header `dataBytes` against a fresh `st_size`, then map
`H + max(window, needed)` bounded by the file.

**R2 — spurious `E_INCOMPATIBLE` under ordinary races (P1, measured three
ways).** A joiner whose fstat lands between the creator's header-page
ftruncate and `EnsureSized` maps the header page only, waits for ready, and
validates `dataBytes` against that stale mapping. Rates: 8/1,600 (8
processes, 1 MiB), 8/600 (2 processes), 8/150 (64 MiB), `Mutex.open` with 8
processes 7/320 and 12/240; concurrent grow 51/90; the grow/join soak
~0.1%. The old build had 0. Ordinary multi-process startup fails
intermittently. **Fix:** after reaching ready, re-fstat and re-map before
validating.

**R3 — silent short window after a lost grow (P1, verified).** When
`GrowSegment` returns 0 the retry loop exits on the refreshed header but never
re-maps; the window is clamped to the old mapping.
`open(n, 1 MiB, {sizePolicy:'grow'})` returned 4096 bytes. Typed-array writes
past the end are silently dropped — data loss, no error. **Fix:** re-map after
a 0 return; throw if the window is smaller than `requested`.

**R4 — `headerBytes` re-read in attach scans (P0 class, race, by reading).**
The ready branch validates `h->headerBytes` (`cc/header.cc:210`), then
`EnsureAttachedRow` → `ClaimAttachRow` recomputes
`AttachSlotCount(h->headerBytes)` from shared memory. Same on the creator
path (write `:169`, scan `:172`), takeover (`:255`/`:258`), and the
`unlinkWhenUnused` scan in `Detach`. A writer that inflates `headerBytes`
between check and scan, with every in-map row showing `refcount > 0`, makes
the scan walk past the mapping and `ClaimAttachRow` CAS and store a 24-byte
identity there. **Fix:** compute the row count once from the validated local
bounded by `mappingBytes` and pass it in.

**R5 — `dataBytes` re-read + unchecked native view (P0 class, race, by
reading).** `dataBytes` is validated in `InitOrJoin` and read again at
`cc/addon.cc:414` and `:431`. A writer showing a small value during
validation and the requested value during the policy check makes `Open`
return a clamped SAB shorter than requested. `Mutex.open` wraps it in an
`Int32Array` (`src/mutex.ts:89`), and `DataAddrOf` (`cc/addon.cc:680-686`)
does not check its length, so `MutexClaimSlot`/`MutexOwnerAlive`/
`ReleaseHeldLock` touch ~2 KiB past the SAB and the mapping. **Fix:** read
`dataBytes` once and return it; throw when the window is smaller than
`requested` (also fixes R3); `DataAddrOf` requires
`byteLength ≥ kMutexDataBytes` (mutex) / ≥ 256 (ring).

**R6 — 0-byte ready segment (P2, reproduced).** A size-less join of a
crashed creator's 0-byte object grace-waits ~280 ms, takes over with
`initBytes = 0`, and publishes a ready 0-byte segment of its own kind. Every
later sized open gets `E_SIZE_MISMATCH` (e.g. a restarted producer: requested
4352 vs existing 0) until someone unlinks. A takeover after a crashed grow
likewise rewrites `dataBytes`/kind. The hardening test covers only
`RingConsumer.open`, which rejects capacity 0 in JS. **Fix:** size-less joins
never initialize (wait or throw `E_NOT_FOUND`/`E_INIT_TIMEOUT`); a takeover
keeps a valid prior header's geometry and kind.

**R7 — multiple initializers (P2, measured).** The takeover winner publishes
`initializerSlot` only after `EnsureSized`; other joiners still see the dead
slot, hand the baton back (1→0) and initialize too. Crafted dead initializer
with 6 concurrent exact joiners: 7/20 runs had >1 initializer, up to 5 sizes
"succeeded", and the final header (16384) was smaller than live SABs
(131072). The creator also stores 1 with a plain store, so a creator stalled
past the grace races a joiner (two `WriteHeader`s). **Fix:** CAS 0→1 together
with publishing the initializer (slot/epoch word); set `initializerSlot`
before any slow work.

**R8 — init-protocol windows (P2).** The joiner's grace path ends with
`ftruncate(fd, headerBytes)` based on an earlier fstat; if a takeover sized
the object in between, this shrinks it and live mappers get SIGBUS. The
grower has a window before recording itself as initializer, and grow error
paths (`EnsureSized` throwing, `cc/header.cc` ~354) can leave state 1 with a
live initializer so joiners wait the full `initTimeoutMs`. The grow loser
polls every 500 µs instead of waiting on the word. **Fix:** grow-only
truncation (re-check size); restore state on every error path; wait on the
word.

**R9 — creator failure strands the name (P1, verified).** If the creator's
`ftruncate`/`posix_fallocate` fails (`ulimit -f` → EFBIG; `E_NO_SPACE`), the
object stays at state 1 with the live creator as initializer. Every open in
any process gets `E_INIT_TIMEOUT` for the creator's lifetime; it cannot be
reaped (magic 0) and the row leaks. The old code unlinked on failure. If
macOS fact U2 holds (an shm object cannot be truncated twice), every non-raw
macOS create fails this way, because the creator now truncates twice
(`cc/segment.cc:255`, then `EnsureSized`). The `E_NO_SPACE` test does not
assert the name was removed. **Fix:** on creator failure unlink and restore;
size in one `ftruncate` on macOS.

**R10 — grow size floor from the header (P2).** `finalBytes =
max(target, HeaderDataBytesOf(h))` (`cc/header.cc:357-358`) feeds
`EnsureSized`/`ftruncate`. With a racing writer (the ready branch rejects a
static hostile value), a victim's grow truncates to an attacker-chosen size,
bypassing `SHM_BRIDGE_MAX_SEGMENT_BYTES`; values ≥ 2^63 become a negative
`off_t` (`E_SYSTEM`). The window stays clamped. **Fix:** bound `existing` by
`st_size - headerBytes` and the cap.

**R11 — teardown SIGSEGV (P1, gdb-verified).** A Mutex claimed after
`unlink` has a null pin; once the instance is dropped and GC'd and the worker
exits, `MutexCleanupIsolate` → `MutexReleaseThreadSlots` walks the unmapped
`claimedData` and segfaults. The same null pin is why F14's
unlink-then-first-lock order still deadlocks. **Fix:** take the pin from the
SAB's BackingStore (`shared_ptr<Mapping>`) and skip entries without one;
treat stale or non-Active tokens as stealable.

**R12 — pin leak (P1, measured).** The claim-time pin lives until thread
teardown even after the Mutex is dropped and the name unlinked. 500 mutexes:
open fds 21 → 521 with 500 mappings live; 5,000 mutexes: 5,000 mappings,
5,000 fds, +40 MB RSS (old build: 1 mapping, 22 fds). tmpfs pages of unlinked
segments are not freed; a lock-per-key pattern on the main thread reaches
`vm.max_map_count` (~65k), and fd exhaustion then triggers R19. **Fix:**
release on GC (FinalizationRegistry → native unregister) or add `close()`.

**R13 — mutex lost-wakeup residual (P2, measured).** `lock()`/`lockAsync()`
skip the `Atomics.or` when the first read already has HAS_WAITERS and park on
a re-read `expected`. A never-waited newcomer can acquire with a bare token in
between, so the waiter parks on a value without the bit and the next unlock
does not notify. Instrumented: every long wait was a no-bit park; 5 µs CS / 0
think → 32 slice timeouts in 4 s, max 502 ms; 6 workers → 2.1 s worst
acquisition. ADR 0005's "no slice-bound handoffs" and the benchmarks.md
3-way row do not hold under tight loops. **Fix:** always
`prev = Atomics.or(word, HAS_WAITERS)`; if `prev` is free, retry; else park
on `prev | HAS_WAITERS`.

**R14 — async ring loses wakeups (P1, measured + source).** Only the sync
paths set `PRODUCER_PARKED`/`CONSUMER_PARKED`; the peer notifies only when
the flag is set. A commit 20 ms into a `peekAsync` wait is seen after
230 ms (8/8; sync `peek` 0.02–0.2 ms); `reserveAsync` the same. 10 s bursty
stream: async consumer 383 msgs/s with 37% of messages over 200 ms; async
producer 38× less throughput; async both sides p50 227 ms. The hardening
async test has no latency assertion (it takes 251 ms). ADR 0006's claim that
the parked wait "usually rides the futex_waitv multiplexer" is wrong — that
is exactly the flagless path. **Fix:** set flag, non-blocking re-check,
`await waitAsync`, clear in `finally`.

**R15 — ring role leftovers (P3).** A crashed waiter's parked flag stays set,
so a busy successor that never parks pays one FUTEX_WAKE per message
forever (reset the flags at role claim). Same-thread `RingConsumer` re-claim
returns the held token, allowing two consumer instances that can process a
message twice.

**R16 — Windows joins fail (P1, by reading).** The new `mode:'join'` path
sets `mappingBytes = 0` (`cc/segment.cc:130`), so the ready-branch check
always throws `E_INCOMPATIBLE`; size-less opens are forced to join, so
`open(name)` and `RingConsumer.open` always fail. The window clamp is also 0
and `FindByAddress` cannot match, so per-word semaphore names fall back to
per-process anonymous names. **Fix:** `VirtualQuery` the view size, as the
whole-object branch does.

**R17 — macOS still does not compile (P1, by reading).**
`os_sync_wait_on_address_with_timeout` is used at `cc/wait.cc:326,349,534,
541,544` but never declared (the extern block at `:214-219` has only
`os_sync_wait_on_address` and `os_sync_wake_by_address_shared`; no
`<os/os_sync_wait_on_address.h>`). The SDK function takes a clockid (6
parameters). Apple exports `os_sync_wake_by_address_any`/`_all`, not
`_shared`; the weak import would resolve to null, so `SyncWake` would wake
nothing and sync waits would sleep full slices. `docs/compat.md` ("implemented
… CI-unverified") overstates this; commit E's "compilation is by
construction" is incorrect. **Fix:** include the SDK header behind an
availability check and use the real signatures — or mark macOS unsupported
until the CI leg passes.

**R18 — liveness cost residual (P3).** `CheckLiveness` still readlinks the
caller's own pid-ns per call (`cc/liveness.cc:116`) and runs on every
contended pass, including right after an `'ok'` wake. **Fix:** use the
cached ns inode; check only after a timed-out slice.

**R19 — failed identity cached (P2, verified).** Transient EMFILE at a
thread's first identity read caches `startTime = -1` permanently: every later
mutex claim on that thread throws `E_SYSTEM` (3/3 even after fds were freed),
attach rows stamped -1 show `'unknown'` forever (never reaped), and role
claims stamp -1 and can never be stolen. **Fix:** do not cache failures;
check `startTime` in role claims too.

**R20 — NaN timeout (P2, verified).** `lock({timeoutMs: NaN})` makes the
deadline and slice NaN, native waits with no timeout and never re-checks
liveness; a SIGKILLed holder hangs it forever (no-timeout `lock()` steals in
501 ms). **Fix:** reject NaN and negatives in mutex/ring options.

**R21 — timeout edge handling (P3).** `SyncWait` maps `!(timeout > 0)` to
1e15 ms (`cc/wait.cc:507-509`); `InitOrJoin` computes `remaining` after its
deadline check (`cc/header.cc:290-291`), so a sub-µs window yields an
effectively infinite park on initState. Commit F's "waitv EINVAL degrades to
a 1 ms park" does not exist — `MuxMain` loops immediately on any errno other
than EAGAIN/ETIMEDOUT; unreachable via the public API now, but a persistent
EFAULT/ENOMEM would hot-spin. **Fix:** non-positive timeout returns timed-out
immediately; add the EINVAL/other-errno park.

**R22 — cost regressions (P3, measured).** Joiners racing a creator pay a
2 ms floor (`usleep(2000)` grace poll; open p50 0.18–0.34 → 2.2–2.3 ms; no
250 ms cases seen) — poll from 50 µs with backoff. Registry reuse costs
2.75 µs (was 1.5–1.7), `Mutex.open` reuse 3.0 µs (was 1.7), from
shm_open+fstat+close per reuse — `fstat(m->fd).st_nlink > 0` detects unlink
with one syscall (still needs the name check for recreate).

**R23 — small native hygiene (P3).** `E_SIZE_MISMATCH`/`E_INIT_TIMEOUT`
thrown after `InitOrJoin` claimed a fresh row leaves the row owned by no
Mapping until process exit (blocks `unlinkWhenUnused`/reap; probably
pre-existing). `std::min(h->headerBytes, …)` (`cc/header.cc:276`) binds a
reference to shared memory — read into a local.

**R24 — documentation drift (P3).**
- PLAN §7.3 still says the steal path sets `ownerDied = 1`.
- PLAN §5.3 says "a joiner never ftruncates"; the grace path and takeover do.
- `cc/wait.cc:193` comment says `Deliver` sends under the lock; it does not.
- `HubFor` (`cc/wait.cc:115`) is dead code (superseded by `node->hub`).
- Bad timeouts throw `E_NAME_INVALID`.
- `docs/benchmarks.md` "Reading them" still says ~2 µs per round trip (its
  table says ~6 µs); the 3-way contended row ("no slice-bound waits") does
  not hold under tight loops.
- ADR 0005 says unlock never clears the bit (unlock CASes to 0 — harmless).
- ADR 0006's multiplexer claim (see R14).
- Commit E/F messages overstate macOS compilation and the EINVAL park.
- No documentation for slot-exhaustion `E_TIMEOUT` (claimed by commit F),
  Windows case-insensitive names (Sec F8), or the `Detach` reap race.

---

## 6. Test gaps

| Gap | Test now | Adequate? |
|---|---|---|
| Ring counters near 2^31 / 2^32 | "Corr F1: ring consumer survives the 2^31 head/tail sign flip" | Partly — 2^31 only |
| Steal without reaping first | "Corr F4: a SIGKILLed-but-unreaped (zombie) holder is stolen from" | Yes (parent never yields a loop turn) |
| Worker-churn mutex slots | "Corr F2: exited workers do not exhaust the 64-slot table" | Graceful exit only; not `terminate()`, claim-after-unlink (R11), pin leak (R12) |
| `ownerDied` on second acquire | "Corr F22: ownerDied is reported exactly once per death" | Yes |
| Worker-churn `waitAsync` | "Corr F3: sequential workers with waitAsync all settle" | Yes (address reuse probabilistic) |
| Create race, different sizes | none ("Corr F5/F26" kills a child 200 ms after init) | No — misses R2, R7 |
| Concurrent grow | "Corr F8" is sequential (and spawns a stray `fork('-e')`) | No — misses R1, R3 |
| Cross-process prefix open | none | No — R1 |
| `lockAsync({signal})` over the cap | none | No (fixed per repro) |
| Init against a real live creator | none (`init-recovery` hand-crafts state 1) | No |
| Async ring latency | "Corr F30" async test, no latency assertion | No — R14 |
| `E_NO_SPACE` leaves no stuck name | `E_NO_SPACE` test does not check | No — R9 |
| Also missing | F12 ring token, F14 order, F20, size-less join of 0-byte plain segment (R6) | No |

---

## 7. What is confirmed good

- All four original P0 hangs (Linux), F9, F11, F16, F21, F22, F24, F28, F29,
  F32, F38.
- Original security P0 class without a race: hostile header, short file,
  takeover indexing, ring geometry — all fail safely.
- Benign race soak: 236k joins, 0 oversized SABs; 6.4k touching joins under
  recreate, 0 SIGBUS.
- Performance: P1, P3, P4, P8, P9, P11 measured; `npm run bench` reproduces
  `docs/benchmarks.md` (Atomics.add 116.7–122.6 M ops/s; handoff
  0.005–0.006 ms; lock/unlock 0.048–0.056 µs; ring 64 B 2.79–3.23 M msgs/s;
  4 KiB 328–388k; 64 KiB 1.9–2.0 GB/s).
- Worker lifecycle: 0 leaked slots after 30 exits; HAS_WAITERS does not stay
  sticky after contention.
