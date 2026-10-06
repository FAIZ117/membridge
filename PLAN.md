# membridge — Implementation Plan (rev 2)

> **Status: implemented (rev 2) — M1–M8 landed. §2 reflects the spike's
> results; `probes/` holds the re-runnable evidence; lasting decisions carry
> ADRs (docs/adr/0001–0004).**
> Name `membridge` verified free on npm (registry 404, checked 2026-10-05).
> Every design decision below carries a **Why** so it can be challenged on its merits.
> §11 lists what changed from rev 1 and the evidence behind each change.

A new npm package with complete ownership that keeps the good parts of
[`shmbuf`](https://www.npmjs.com/package/shmbuf) (MIT, by kedemd) while fixing its
bugs and adding real cross-process synchronization. membridge supersedes shmbuf for our use.

**Scope (locked):** TypeScript · crash-safe Mutex · zero-copy RingBuffer · ops utilities.
**Platforms:** Linux (primary), macOS and Windows (first-class, fully tested in CI — not "smoke").

---

## 1. Location & ownership

- Standalone git repo at `/home/faiz/work/membridge/` (this repo).
- MIT LICENSE, author "Faiz" (placeholder — edit before publishing).
- README credits shmbuf (MIT) as the starting point.
- Publishing to npm is done by the owner, not by the agent. `package.json` keeps
  `"private": true` until then.

## 2. Verified facts driving the design

Each fact was checked on the dev machine (Node 24.18.0, Linux 7.2.8, glibc 2.42,
page size 4096) against shmbuf 0.1.0 at
`~/work/trial-node-memory/node_modules/shmbuf`.

| # | Fact | Evidence |
|---|------|----------|
| F1 | **`Atomics.notify` does not wake waiters in another process.** V8 keeps waiters in a per-process list; it does not use a shared kernel futex. | Probe: child `Atomics.wait(v,0,0,3000)` on a shmbuf segment, parent stores + `Atomics.notify` after 200 ms → `notify()` returned **0**, child returned `'timed-out'` after **3001 ms**. |
| F2 | Wrapping mmap'd memory as a `SharedArrayBuffer` requires direct V8 (`v8::SharedArrayBuffer::NewBackingStore` + deleter); N-API has no API for it. | shmbuf `src/shmbuf.cpp:51-55`; N-API headers for Node 18–24. |
| F3 | shmbuf mixes N-API and raw V8 by casting `v8::Value*` to `napi_value` — this relies on Node internals, not a public contract. | `src/shmbuf.cpp:58-59`. |
| F4 | shmbuf returns a handle created inside a plain `v8::HandleScope` after that scope is destroyed (dangling handle; works by luck). | `src/shmbuf.cpp:49-59`. |
| F5 | shmbuf converts size with `Uint32Value()`, silently wrapping. | Probe: `open(n, 2**32+16).byteLength === 16`; `open(n, 1.9).byteLength === 1`. |
| F6 | Re-joining an existing segment with a larger size succeeds and maps past EOF (touching pages beyond EOF → SIGBUS). | Probe: `open(n,64)` then `open(n,128).byteLength === 128`, no error. |
| F7 | `fallocate` on tmpfs charges pages up front (so we can get `ENOSPC` at create time instead of a later SIGBUS). | `fallocate -l 8M /dev/shm/x` → `blocks=16384` (8 MiB charged). |
| F8 | `/dev/shm` = 7.6 GB tmpfs here (default 50% of RAM, lazily charged). shmbuf's 64 MB cap is arbitrary (`src/shmbuf.cpp:71`). | `df -h /dev/shm`. |
| F9 | Liveness primitives exist on Linux: `/proc/<pid>/stat` field 22 (start time), `/proc/self/ns/pid` (pid-namespace inode). | Read both on dev machine. |
| F10 | shmbuf's perf test times a ~61 ms run with a 10 ms poll and prints the same rate for both phases; it is not a usable regression gate. | `test/test-shmbuf.js:168-192`; local run: 61 ms elapsed. |
| F11 | shmbuf is **not** zero-dependency: it depends on `node-addon-api` and `prebuild-install`, and its install script uses `prebuild-install --runtime napi` — wrong for a V8-ABI-bound addon. Its loader only tries `build/Release`. | shmbuf `package.json`, `lib/index.js:5`. |
| F12 | Toolchain present: gcc/g++ 15.3, make, python3; node-gyp header caches for 18–24. Node 26.10.0 headers are fetched on demand by node-gyp (nodejs.org reachable) and the Node 26.10.0 runtime was verified from the official tarball. | `ls ~/.cache/node-gyp`; M1 spike built and passed on 22.22.0, 24.18.0, 24.19.0, 26.10.0. |
| F13 | Official Node builds do not enable the V8 sandbox, so external backing stores work. Electron does enable it → Electron is unsupported (README compatibility note, not a plan risk). | `node -p process.config.variables.v8_enable_sandbox` → `0` on 24.18.0; shmbuf's tests pass on it. |
| F14 | `futex_waitv` (Linux ≥ 5.16) has **no glibc wrapper**: glibc 2.42 exports no futex symbols at all. It is reachable only as a raw syscall (`__NR_futex_waitv` = 449 in `asm-generic/unistd.h`; `struct futex_waitv` in `linux/futex.h`). | `nm -D /lib64/libc.so.6 \| grep futex` → nothing; `grep -rn futex_waitv /usr/include`. **M1 probe** (`probes/f14-shared-futex.js`): shared `FUTEX_WAIT`/`FUTEX_WAKE` (no `FUTEX_PRIVATE_FLAG`) wakes across processes on kernel 7.2.8; `futex_waitv` wakes cross-process with `FUTEX2_SIZE_U32` and no `FUTEX2_PRIVATE`, and returns the **index of the woken entry** (0-based), not a count — the fact the §6 multiplexer needs. |
| F15 | A SAB's `byteLength` always equals its BackingStore's length — a SAB cannot be a window onto a larger store. | `v8::SharedArrayBuffer::New(isolate, std::shared_ptr<BackingStore>)` takes the length from the store; M1 probe exercises the `base + headerBytes` window (`probes/f2-sab-window.js`). |
| F16 | V8 14.6 (Node 26) removed `v8::Context::GetIsolate()`. Plain-V8 addons get the isolate with `v8::Isolate::GetCurrent()` inside `NODE_MODULE_INIT` (function callbacks keep `args.GetIsolate()`). | Spike build on 26.10.0 failed with the removal; fixed in `spike/addon.cc`; full spike passes on 26.10.0. |
| F17 | `Worker.terminate()` resolves promptly (~3 ms) while the worker is parked in `Atomics.wait` on a custom-BackingStore SAB — isolate teardown is clean, as §6's lifecycle design assumes. | `spike/u5-terminate.js` on Node 24.18.0 and 26.10.0. |
| U4 | **Confirmed (M1):** Node 26.10.0 is the current release (checked nodejs.org/dist 2026-10-06; V8 14.6, ABI 147) and `SharedArrayBuffer::NewBackingStore(ptr, len, deleter, data)` is unchanged there. Only adaptation: F16. | M1 spike builds and passes all scripts on 26.10.0. |
| U5 | **Confirmed (M1):** in-process `Atomics.wait`/`Atomics.notify` pair across *distinct* BackingStores covering the same mapping — same isolate (`window`) and across worker isolates (worker's own `open()`); V8 keys its waiter list by address, not by BackingStore. §5.4 keeps the per-open BackingStore design; no shared-BackingStore fallback needed. | `probes/u5-atomics-pairing.js` — waiter wakes on the first cross-store `notify` (~250 ms) in both cases. |

Facts still **unverified** (need foreign runners; the design implements the
documented fallback for each in M3 regardless — a local spike cannot check them):

- U1 macOS: `os_sync_wait_on_address` with `OS_SYNC_WAIT_ON_ADDRESS_SHARED` (macOS 14.4+) wakes across processes.
- U2 macOS: POSIX shm names are limited to 31 chars (`PSHMNAMLEN`), and an shm object cannot be `ftruncate`d again once sized.
- U3 Windows: `WaitOnAddress` is process-private (per Microsoft docs), so cross-process waits need named kernel objects.

## 3. Architecture overview

```
 JS (TypeScript)                                   Native (C++, plain V8 / node.h addon)
 ─────────────────                                  ─────────────────────────────────────
 open/close/unlink  ──► core.ts ──────────────────► segment.cc   shm_open/ftruncate/fallocate/mmap
                                                                 CreateFileMapping/MapViewOfFile
                                                    registry.cc  process-wide name → weak_ptr<Mapping>
 Mutex   ──► mutex.ts ─┐                            header.cc    segment header + attach table
 RingBuffer ► ring.ts ─┼─► sync.ts (wait/notify) ─► wait.cc      futex (Linux) / os_sync (macOS) /
 list/stat/reap ► ops.ts                                         named semaphores (Windows)
                                                    liveness.cc  pid + start time + pid-ns identity
```

Every membridge segment is **header page + data region**. The SAB handed to the
user covers only the data region, so user offsets start at 0 and stay page-aligned.

**Mapping vs. BackingStore (explicit, to avoid an M2 mistake):** because a SAB's length
always equals its BackingStore's length (F15), the window is made at the BackingStore level:
- A native `Mapping` object owns the OS mapping: `base` (what `mmap`/`MapViewOfFile` returned)
  and `mappingBytes = headerBytes + dataBytes`.
- Each BackingStore is created as `NewBackingStore(base + headerBytes, sabBytes, deleter, ref)`,
  where `ref` is a heap-allocated `shared_ptr<Mapping>`. The deleter only drops that reference.
- `~Mapping()` calls `munmap(base, mappingBytes)` / `UnmapViewOfFile(base)` + `CloseHandle`
  — always on **`base`**, never on the pointer the BackingStore was given.

This also means several windows (e.g. an `at-least` prefix and a full view) can share one mapping.

## 4. Native addon: plain V8 addon, not N-API

**Decision:** write the addon against `node.h`/`v8.h` (`NODE_MODULE_INIT`, context-aware),
and drop `node-addon-api`.

**Why:** the core technique needs raw V8 (F2), which ties the binary to one Node ABI
anyway. Keeping N-API on top gives none of its ABI stability, but still needs the
undocumented `v8::Value*`→`napi_value` cast (F3) and a dependency. A plain V8 addon
returns `v8::Local` directly, so the cast and the handle-scope bug (F4) disappear
by construction. Context-aware init (`NODE_MODULE_INIT`) is required for `worker_threads`.

## 5. Core segments

### 5.1 API

```ts
open(name: string, size: number, opts?: OpenOptions): SharedArrayBuffer
open(name: string, opts: OpenOptions & { size?: undefined }): SharedArrayBuffer // join at existing size

interface OpenOptions {
  mode?: 'create-or-join' | 'create' | 'join';   // default 'create-or-join'
  sizePolicy?: 'exact' | 'at-least' | 'grow';    // default 'exact'
  reserve?: boolean;        // Linux: fallocate the data region at create. default true
  permissions?: number;     // POSIX mode bits at create. default 0o600
  initTimeoutMs?: number;   // how long a joiner waits for the creator to finish. default 5000
  raw?: boolean;            // no header: foreign-process interop, loses all header features. default false
}
close(name: string): void   // compatibility no-op, see §5.4
unlink(name: string): void
isNative(): boolean
```

`mode: 'create'` throws `E_EXISTS` if present; `'join'` throws `E_NOT_FOUND` if absent.

### 5.2 Segment header (one page, at offset 0)

| Field | Type | Purpose |
|-------|------|---------|
| magic, layoutVersion | u32, u32 | Reject non-membridge or incompatible segments (`E_INCOMPATIBLE`). |
| initState | i32 | 0 = uninit, 1 = initializing (+ initializer slot), 2 = ready. Joiners wait on it with native wait (§6); every transition wakes the word. **Protocol (fix round 2026-10-06):** the creator ftruncates the header page immediately after `O_EXCL`, publishes `headerBytes` + its attach row + `initializerSlot`, stores 1, and only then sizes the object fully and stores 2 — a joiner never sees a live creator at state 0. A joiner at state 0 grace-waits (250 ms in `OpenSegment` for the header page, 50 ms for the state word), then wins a 0→1 CAS to initialize. A dead initializer at state 1 is handed back through a 1→0 CAS so exactly ONE joiner wins the 0→1 CAS — no concurrent-WriteHeader war. Takeover is idempotent and rewrites every field. |
| headerBytes, dataBytes | u32, u64 | Authoritative size — works the same on all OSes (Windows has no `fstat` for sections). |
| flags | u32 | e.g. `ATTACH_OVERFLOW`, `KIND` (raw / mutex / ring). |
| attach table | N × 32 B | One slot per attached process: identity (§7.1) + local refcount. ~120 slots with a 4 KiB page. |

**Why a header:** it fixes, in one mechanism, four problems rev 1 handled badly or
not at all:
1. **Create/join race:** a joiner that arrives before the creator finishes waits for
   `initState == 2` instead of reading `st_size == 0` and failing.
2. **Windows size check:** the size is read from the header (rev 1's `fstat` fix was POSIX-only).
3. **Reliable `list/stat/reap`:** we know who is attached without guessing.
4. **Crash recovery of a half-initialized segment:** if `initState == 1` and the
   initializer is dead (§7.1), a joiner takes over initialization.

**Cost:** segments are not byte-compatible with shmbuf or foreign tools. `raw: true`
exists for that case (no header, so no size check beyond `fstat`, no attach table).

**Header size:** `max(4096, system page size)` so the data region stays page-aligned
(every TypedArray alignment works, and 16 KiB pages on Apple Silicon are respected).

### 5.3 Size handling

- **Validation first, in JS and native:** `Number.isSafeInteger(size) && size >= 1 && size <= maxSegmentBytes`.
  Native reads the size as `double`/`int64`, never `Uint32Value()` (F5).
- **Policies on join** (compared against `header.dataBytes`):
  - **The mapping is the single source of truth for geometry (fix round 2026-10-06).** Joins never map more bytes than the object actually holds (`min(requested, st_size)`; whole-object joins map `st_size`), a joiner never `ftruncate`s, and every header-derived size is validated/clamped against `mappingBytes − headerBytes`: a ready join with `header.dataBytes` above that bound is `E_INCOMPATIBLE`. This is what makes a hostile or stale header unable to size a `BackingStore` past mapped memory.
  - `exact` (default): mismatch → `E_SIZE_MISMATCH` naming both sizes.
  - `at-least`: requested ≤ existing → map the requested prefix; larger → `E_SIZE_MISMATCH`. **Why:** mapping a prefix can never SIGBUS, and lets a reader map only a known header.
  - `grow`: requested > existing → under the header's init lock `ftruncate` to `max(requested, file size − header)` (never shrink below a concurrent grower), update `dataBytes`, and re-map so the mapping covers the new size. A grower that loses the init-lock race retries the policy against the refreshed header. Processes already attached keep their smaller SAB (a SAB cannot be resized in place) — documented. On macOS, if U2 holds, → `E_GROW_UNSUPPORTED`. **On Windows always `E_GROW_UNSUPPORTED`:** sections are fixed at `CreateFileMapping` time and cannot be resized, so `grow` is a POSIX-only policy; `docs/compat.md` states it.
- **Reserve (Linux):** `posix_fallocate` on the data region at create; failure → `E_NO_SPACE`.
  **Why:** tmpfs charges lazily, so a full `/dev/shm` shows up as SIGBUS on first touch,
  possibly hours later. Reserving turns that into a create-time error (F7).
  **Cost:** memory is committed up front; set `reserve: false` for sparse use.
- **Windows:** pass the high DWORD to `CreateFileMappingW` (shmbuf hard-codes 0 → 4 GiB ceiling),
  check `ERROR_ALREADY_EXISTS` to tell create from join.
- **Size cap:** default 256 MiB, overridable via `MEMBRIDGE_MAX_SEGMENT_BYTES`.
  **Why** a cap at all: a typo (e.g. bytes vs. KB) should not silently commit gigabytes when `reserve` is on.

### 5.4 Lifecycle and registry

- Native, process-wide `std::mutex`-guarded map `name → weak_ptr<Mapping>` (§3).
  `open()` in any isolate (main or worker) reuses a live `Mapping` and creates a **new
  BackingStore + SAB over it**, so there is no re-mmap.
  **Why not "return the same SAB" (rev 1):** JS objects cannot cross isolates, and holding a
  strong JS reference to return later would stop the SAB from ever being GC'd.
  `shared_ptr<Mapping>` gives the correct lifetime for free: the mapping lives while any SAB
  in any isolate holds it.
- **Registry lookup with sizes:** a cached `Mapping` is reused only if its `dataBytes` covers
  the requested size under the active policy. If the segment was grown (by this or another
  process) and a larger size is requested, a **new** `Mapping` is made and replaces the
  registry entry; SABs over the old mapping stay valid, because they hold their own reference.
  **Why:** otherwise `grow` would silently hand back the stale, smaller mapping inside one process.
- **Registry lookup validates object identity (fix round 2026-10-06):** reuse additionally
  compares the cached Mapping's recorded `dev`/`ino` against a fresh `shm_open`+`fstat` of the
  name; a mismatch (another process unlinked and recreated the name) drops the entry and takes
  the full-open path. **Why:** otherwise a long-lived process would keep handing out the old
  mapping while fresh processes use the new object — a silent split brain (review F16). The
  same identity check guards `unlinkWhenUnused` (only unlink when the name still refers to our
  object, F21), and a second Mapping over the same segment (post-grow) shares the attach row —
  only the process's last Mapping releases it (F20). Windows sections carry no inode; reuse is
  name-keyed there and documented.
- `~Mapping()` (unmap + release this process's attach slot) can run on **any thread**,
  because V8 may call BackingStore deleters off the main thread. Registry and header updates
  in it are therefore lock-protected / atomic.
- **`close(name)` is a documented compatibility no-op** (it only validates the name, so shmbuf
  call sites keep working). **Why:** it cannot unmap (that would be use-after-free under a live
  SAB); the registry is process-wide and holds only weak references, so there is no
  per-isolate state for it to release; and the size-aware lookup above removes the one
  reason to force a fresh mapping. Memory is released when the last SAB is GC'd; the name is
  removed by `unlink()` or `unlinkWhenUnused`. Rev 1's "close becomes meaningful" is withdrawn.
- `unlink(name)` removes the name (POSIX `shm_unlink`). On Windows the section
  disappears when the last handle closes; `unlink` there only prevents new joins
  via membridge (marks the header `UNLINKED`).

### 5.5 Names

- Validation per platform: POSIX names start with `/`, have no other `/`, and are
  ≤ 250 bytes on Linux. On macOS the limit is `PSHMNAMLEN` (31 if U2 holds).
  On Windows the name is mapped to `Local\membridge<escaped-name>`, where each `/`
  becomes `%2F` and a literal `%` becomes `%25` (percent-encoding — a plain `_`
  replacement would collide `/a_b` with `/a/b`); a result exceeding the kernel's
  object-name limit is `E_NAME_INVALID`, not silently truncated. An opt-in `Global\`
  prefix is available (needs `SeCreateGlobalPrivilege`).
- **Why per-platform:** a name accepted on Linux but rejected on macOS should fail with
  `E_NAME_INVALID` at the call site, not with a raw `ENAMETOOLONG` from deep inside.

### 5.6 Fallback (no native addon)

- Opt-in only: `MEMBRIDGE_ALLOW_FALLBACK=1` or `open(..., { allowFallback: true })`.
  Otherwise, a missing addon → `E_NATIVE_UNAVAILABLE`.
- In fallback, `Mutex` and `RingBuffer` throw `E_NATIVE_UNAVAILABLE` unless explicitly opted in.
  Fallback still checks sizes (shmbuf ignored size on re-open).
- **Why:** a fallback that silently "works" inside one process turns a missing build
  into a cross-process correctness bug that only appears in production.

## 6. Cross-process wait/notify (new native primitive)

```ts
sync.wait(view: Int32Array, index: number, expected: number, timeoutMs?: number): 'ok' | 'not-equal' | 'timed-out'
sync.waitAsync(view, index, expected, timeoutMs?): Promise<'ok' | 'not-equal' | 'timed-out'>
sync.notify(view: Int32Array, index: number, count?: number): void
```

**Why:** F1 — `Atomics.notify` cannot wake another process, so a Mutex or RingBuffer
built on `Atomics.wait` degrades to polling at its timeout interval (rev 1's 50 ms
loop = up to 50 ms per contended handoff). This is the foundation for everything in §7–§8.

| OS | Mechanism | Notes |
|----|-----------|-------|
| Linux | `futex(FUTEX_WAIT / FUTEX_WAKE)` **without** `FUTEX_PRIVATE_FLAG` on the mapped address | Shared futexes are keyed by the underlying page, so they work across processes. 32-bit words only → every waitable word in our layouts is `i32`. |
| macOS | `os_sync_wait_on_address` / `os_sync_wake_by_address_*` with the SHARED flag (14.4+) | U1. If the spike fails or the OS is older: bounded exponential back-off polling (50 µs → 2 ms), documented as higher latency. |
| Windows | Per-word named semaphore `Local\membridge-<seg>-w<offset>`; the semaphore's permits are the waiter bookkeeping (no separate shm count) | U3. Notify releases `count` permits — more than waiters is fine: extra permits surface as the spurious wakeups this row already allows, and every waiter re-checks the word. |

- **Sync waits** block the calling JS thread (same as `Atomics.wait`) — documented;
  use `waitAsync` on a server main thread.
- **Async waits** run on a membridge-owned native waiter thread, **not the libuv
  threadpool**. **Why:** the default pool has 4 threads; a few pending lock waits would
  starve `fs`/`dns`/`crypto`. On Linux ≥ 5.16 one thread can multiplex up to 128
  waits via `futex_waitv`; elsewhere one thread per pending wait, capped
  (`E_TOO_MANY_WAITERS`). Results are delivered with a threadsafe callback.
- **Wait lifecycle pins memory and ends with the isolate.** A pending wait holds a
  strong reference to the `Mapping` (§3) for its duration, so GC cannot `munmap`
  the page under a sleeping futex. Isolate teardown (worker exit or `.terminate()`)
  cancels that isolate's pending waits, wakes the native waiter, and resolves each
  promise as `'timed-out'`; the §7.1 env-cleanup hook covers waits as well as
  locks. **Why:** a waiter sleeping on a page a finalizer unmaps is a
  use-after-free, and a promise that never settles leaks an uncollectable handle.
- **Windows wakeups are best-effort; correctness never depends on one.** A waiter
  that increments the shm waiter-count after `notify()` has read it misses the wake
  until its bounded timeout. Every loop in §7–§8 therefore re-checks the word on
  each wake *and* on timeout — a missed wake costs latency (≤ the bounded
  timeout), never correctness.
- **`futex_waitv` has no glibc wrapper (F14):** M3 calls it as
  `syscall(__NR_futex_waitv, waiters, n, 0, &abs_timeout, CLOCK_MONOTONIC)`, with each
  `struct futex_waitv` entry using `FUTEX2_SIZE_U32` and **without** `FUTEX2_PRIVATE`
  (the waits must be shared). The same goes for plain `futex` (raw `syscall(SYS_futex, …)`;
  glibc has no wrapper for that either). If the build headers lack `__NR_futex_waitv`, define
  it as 449 (same value on x86_64 and asm-generic arches such as arm64). `ENOSYS` at runtime
  (kernel < 5.16) → one thread per wait. musl builds use the same raw-syscall path.
  The multiplexer is woken to change its wait set by a private control futex in its own wait vector.
- Exposed publicly (`membridge/sync`) because users building their own lock-free
  structures on a membridge SAB hit F1 just the same.

## 7. Crash-safe Mutex

### 7.1 Participant identity (shared with RingBuffer and reap)

An identity is `{ pid, startTime, pidNsInode, threadId }`:
- **startTime** (Linux `/proc/<pid>/stat` field 22; macOS `proc_pidinfo`; Windows
  `GetProcessTimes`). **Why:** PIDs get reused; pid alone would let a recycled PID
  look like a live owner, or a new process look like the dead one.
- **pidNsInode** (Linux `/proc/self/ns/pid`, F9). **Why:** containers that share
  `/dev/shm` (`--ipc=...`) see different PIDs for the same process. If the namespace
  differs from ours, liveness is **unknowable → treat as alive, never steal**, and
  document that recovery across PID namespaces needs a process in the owner's namespace.
- **threadId** (Node `worker_threads.threadId`). **Why:** all workers share one PID,
  so pid alone cannot tell which thread holds a lock or distinguish a dead worker from a live sibling.

Liveness check: `kill(pid,0)` (`ESRCH` = dead) then one `/proc/<pid>/stat` read that must
succeed and match — state `Z`/`X` (zombie) counts as **dead** (a zombie answers `kill(pid,0)`
as alive and would keep a holder unstealable forever; fix round 2026-10-06), a start-time
mismatch counts as dead (pid recycled), and an unreadable stat (hidepid) counts as **unknown →
never steal**. An identity whose start time was never recorded (`-1`) is never stealable either.
Windows `OpenProcess`+`GetExitCodeProcess`. Self-identity (startTime, pid-ns inode) is cached
per thread and refreshed when the pid changes; a claim with an unrecordable identity throws
`E_SYSTEM` rather than stamping a slot that later misjudges its live owner.

**Worker death inside a live process:** the addon registers an env-cleanup hook per
isolate (at slot-claim time — one native registration per Mutex instance, zero per
lock/unlock; fix round 2026-10-06). When a worker exits or is terminated, the hook
releases anything the thread still holds (marking `OWNER_DIED` and waking a waiter),
clears its ring-role claims, **and frees the thread's participant slots** — an exited
worker used to leak its slot permanently and exhaust the 64-row table. **Why:** a
process-liveness check cannot detect a dead thread in a live process, and slot
reclamation only sees dead *processes*.

### 7.2 Layout

Segment `kind = mutex`, data region:

```
i32 lockWord     0 = free; otherwise token = (slotIndex << 16) | gen15,  bit 31 = HAS_WAITERS
i32 ownerDied    set when a steal happens; reported to the next holder
i32 seq          incremented per acquisition (diagnostics)
slots[64]        { identity (24 B), gen: i32, state: i32 } — one per participant thread
```

**Why a single 32-bit token word:** acquisition, release and **stealing are all a single CAS
on one futex-compatible word**. Rev 1 kept state/owner/generation in separate words, so a
steal could not be atomic. The token points at a participant slot whose identity was
written **before** the CAS, so a holder that crashes between "acquire" and "record owner"
cannot exist.

**Slot reclamation:** the env-cleanup hook (§7.1) frees a thread's slot on graceful
exit, but a SIGKILLed process's slot would linger and exhaust the 64-slot table
under churn. A thread that finds no free slot scans for one whose identity fails
the §7.1 liveness check **and whose token is not the current `lockWord` value** —
a dead *holder's* slot is freed by the steal path first (the contender steals the
lock into its own already-claimed slot, after which the dead slot is unreferenced)
— and claims it with a single CAS on `slot.gen`, bumping gen. **Why CAS on gen:**
bumping gen instantly invalidates every outstanding token pointing there (the same
mechanism that makes stealing safe); a live owner's slot always passes the liveness
check so it is never evicted; and a stale token can never win a CAS again.

### 7.3 Protocol

- **Lock:** CAS `0 → myToken` — a whole-word write, so a stale HAS_WAITERS bit a lost
  race left behind is cleared. On failure: set `HAS_WAITERS`, **re-read the word and retry
  immediately if it now reads free**, then `sync.wait(lockWord, observed)`; after the first
  park, acquisition writes `myToken | HAS_WAITERS` — the bit is preserved because other
  waiters may still be parked on the word the new owner will unlock (Drepper's mutex2; fix
  round 2026-10-06, ADR 0005 — the bare-token acquire erased the bit and waiters slept
  full slices on a free lock). Every wait has a bounded timeout (default 250 ms, clamped to
  the caller's remaining timeout) after which the owner's liveness is checked.
  **Why a timeout at all:** a dead owner never calls `notify`.
- **Steal:** owner token → slot → identity is dead (§7.1) and slot gen matches the token
  → CAS `deadToken → myToken`, set `ownerDied = 1`. Losing the CAS just means retry.
- **No heartbeat.** **Why:** the owner cannot update a heartbeat while inside a synchronous
  critical section (the event loop is blocked), so a heartbeat timeout would steal from a
  live, busy owner. Stealing on liveness alone is both safe and sufficient.
- **Unlock:** verify `lockWord` holds my token (else `E_NOT_OWNER`), store `0`, notify one if `HAS_WAITERS`.
- **Owner-died is reported, not hidden** (the equivalent of pthread `EOWNERDEAD`):
  `lock()` returns `{ ownerDied: boolean }`; `withLock(fn)` passes it to `fn`.
  **Why:** the protected data may be half-updated; only the caller can repair it.
- **Re-entrancy:** locking twice from the same thread → `E_DEADLOCK` (not a silent hang).
- **No priority inheritance, no fairness guarantee:** a waiter woken by `notify` competes
  with newcomers for the CAS, and a low-priority holder is never boosted. Fine for v1;
  the README says so explicitly so nobody uses it for latency-critical control loops.
  (Linux `FUTEX_LOCK_PI` would require the kernel's TID-based word format, which conflicts
  with the token design and does not exist on macOS/Windows.)
- **API:** `Mutex.open(name)`, `lock({timeoutMs})`, `tryLock()`, `lockAsync({timeoutMs, signal})`,
  `unlock()`, `withLock(fn)`, `withLockAsync(fn)`. Timeout → `E_TIMEOUT`.

**Alternative considered: OS robust mutexes** (Linux `PTHREAD_MUTEX_ROBUST`, Windows named
mutex `WAIT_ABANDONED`). Rejected as the primary mechanism: macOS has no robust mutexes
(no single design across platforms), an OS mutex must be released by the thread that
locked it (incompatible with `lockAsync`, whose wait runs on another thread), and glibc
owns the per-thread kernel robust list, so we cannot register our own futex words with it.
The liveness-based design above gives the same guarantees (detects a dead owner, reports
it, never deadlocks) on all three OSes.

## 8. Zero-copy RingBuffer (SPSC, variable-length messages)

### 8.1 Layout

Segment `kind = ring`, data region:

```
i32 head        producer's committed byte count (free-running, wraps mod 2^32)
i32 cParked     consumer-parked flag (padding word, head's cache line — ADR 0006)
i32 tail        consumer's released byte count (free-running)
i32 pParked     producer-parked flag (padding word, tail's cache line)
i32 producer    participant token (role claim)
i32 consumer    participant token (role claim)
u32 capacity    power of two, 4 KiB … 1 GiB
u32 maxMessage  ≤ capacity/2 - 8
… rest of the two cache lines as padding
data[capacity]
```

The parked flags (fix round 2026-10-06, ADR 0006): the waiting side sets its
flag, re-checks, parks, and clears it on exit; the notifying side wakes only
when the peer's flag is set — an unparked message costs zero FUTEX_WAKEs. A
crashed waiter leaves its flag set, costing one futile wake per notify, never
correctness.

- **Free-running 32-bit counters with power-of-two capacity**: `used = (head - tail) >>> 0`.
  **Why:** no "one empty slot" waste (Lamport), and the counters stay `i32`, so they are
  waitable (§6). That caps capacity at 1 GiB — rev 1's "~2 GB" needed `Uint32Array`, which
  `Atomics.wait` and futex helpers do not accept, and contradicted the 256 MiB segment cap anyway.
- **Capacity vs. the segment cap:** a ring needs `headerBytes + capacity`, so any
  capacity above the current `MEMBRIDGE_MAX_SEGMENT_BYTES` (default 256 MiB)
  throws `E_SIZE_INVALID` until the env override is raised. **Why not clamp
  silently:** a producer configured for 512 MiB that silently gets 256 MiB has
  its backpressure behavior changed under it; refusing is honest.
- **Framing:** each message = `u32 length` + payload, padded to 8 bytes. If a message does not
  fit before the end of the buffer, the producer writes a `SKIP` marker and the message
  starts at offset 0. **Why `maxMessage ≤ capacity/2`:** guarantees that a message always
  fits contiguously once the buffer drains, so a skip can never deadlock.

### 8.2 API (zero-copy two-phase, plus copying conveniences)

```ts
const p = RingBuffer.producer(name, { capacity, maxMessage });
const view = p.reserve(n, { timeoutMs }); // Uint8Array straight into shared memory
view.set(...); p.commit();                // publishes with a release store on head + notify
p.write(bytes)                            // convenience = reserve + copy + commit (one copy)

const c = RingBuffer.consumer(name);
const msg = c.peek({ timeoutMs });        // Uint8Array view into shared memory, or null
use(msg); c.release();                    // only now does tail advance
c.read()                                  // convenience = peek + copy + release
// async variants: reserveAsync / peekAsync (via sync.waitAsync)
```

- **Why two-phase:** rev 1's `write(view)` copied, and a `read()` returning a view was unsafe
  because the producer may overwrite those bytes as soon as `tail` advances. With
  `peek/release` the bytes stay valid until the consumer says so. Calling `peek`/`reserve`
  again before `release`/`commit` → `E_RING_STATE`.
- **Blocking:** producer waits on `tail` when full; consumer waits on `head` when empty (§6).
- **SPSC enforced:** `producer()`/`consumer()` claim the role with a CAS on the role word.
  A live holder → `E_ROLE_TAKEN`; a dead holder (§7.1) is replaced.
- **Crash safety by construction:** data becomes visible only through `commit()`'s store
  to `head`. A producer that dies mid-`reserve` leaves nothing visible; a consumer that dies
  mid-`peek` leaves the message unreleased, so the next consumer sees it again
  (**at-least-once** — documented).
- **Init:** via the segment header's `initState` (§5.2); `capacity`/`maxMessage` given by a
  joiner must match the header or → `E_SIZE_MISMATCH`.

## 9. Ops utilities

| API | Behavior | Platforms |
|-----|----------|-----------|
| `capacity()` | free/total of the shm backing store (`fs.statfs('/dev/shm')`) | Linux. macOS/Windows: `E_UNSUPPORTED` (no bounded shm filesystem). |
| `stat(name)` | header contents: kind, sizes, creator, attached identities + liveness, owner of mutex/ring roles | all |
| `list()` | Linux: readdir `/dev/shm`, keep entries whose header has membridge magic (**Why** magic, not a name prefix: users choose names; the header is the reliable marker). macOS: `E_UNSUPPORTED` (POSIX shm cannot be enumerated). Windows: `E_UNSUPPORTED`. | Linux |
| `reap({ dryRun })` | unlink segments whose every attach slot is dead and that have no `ATTACH_OVERFLOW` flag. Unknown liveness (foreign pid namespace) counts as alive. | Linux (needs `list`); `reap(name)` for a single segment on all OSes |
| `open(..., { unlinkWhenUnused: true })` | the last process to detach (attach table becomes empty) unlinks the name | all |

**Why this replaces rev 1's `cleanupOnExit`:** exit handlers do not run on SIGKILL, SIGBUS
or OOM-kill (nor on SIGINT/SIGTERM without handlers), so they cannot clean up after
crashes. And unlinking on the *first* exit while others still use the segment makes later
joiners create a separate segment with the same name — two processes silently using
different memory. Ref-counting in the attach table plus `reap()` covers both cases correctly.

## 10. Errors

`MembridgeError extends Error` with `code`, plus structured fields (`segmentName`, `requested`, `existing`, `syscall`, `errno`; "segmentName" rather than `name` because `Error.name` already carries the error-class name):

`E_NAME_INVALID` · `E_SIZE_INVALID` · `E_SIZE_MISMATCH` · `E_EXISTS` · `E_NOT_FOUND` ·
`E_INCOMPATIBLE` · `E_INIT_TIMEOUT` · `E_NO_SPACE` · `E_GROW_UNSUPPORTED` · `E_NOT_OWNER` ·
`E_DEADLOCK` · `E_TIMEOUT` · `E_ROLE_TAKEN` · `E_RING_STATE` · `E_MESSAGE_TOO_LARGE` ·
`E_TOO_MANY_WAITERS` · `E_NATIVE_UNAVAILABLE` · `E_UNSUPPORTED` · `E_SYSTEM` (wraps errno/GetLastError with the syscall name).

## 11. Changes from rev 1 and why

| Rev 1 | Rev 2 | Reason |
|-------|-------|--------|
| Mutex waits with `Atomics.wait` + 50 ms loop | Native cross-process wait/notify (§6) | F1: cross-process notify does not work; rev 1 was polling |
| Heartbeat + pid check, steal via separate gen word | Identity = pid + start time + pid-ns + threadId; single-word token CAS; no heartbeat | Heartbeat steals from live owners stuck in sync code; PID reuse/containers/workers; separate words made stealing non-atomic |
| Steal silently | `ownerDied` reported to the next holder | Protected data may be inconsistent |
| `write(view)` / `read()` ring, bytes unspecified | Two-phase `reserve/commit`, `peek/release`, length-prefixed framing, role claims | Rev 1 was neither zero-copy nor safe; framing/blocking/SPSC/crash handling were unspecified |
| Ring capacity ~2 GB with u32 head/tail | Power-of-two ≤ 1 GiB, free-running i32 counters | Waitable words must be i32; contradicted the 256 MiB cap |
| `fstat` size check (POSIX only) | Size in segment header + init state | Works on Windows; fixes create/join race instead of making it a spurious error |
| "Document" SIGBUS on full tmpfs | `posix_fallocate` reserve → `E_NO_SPACE` | F7: fixes the hazard rather than describing it |
| Same SAB returned for same name | Weak registry of `Mapping`s; new BackingStore + SAB per open | SABs cannot cross isolates; strong refs block GC; SAB length = store length (F15) |
| `close()` "becomes meaningful" | Documented compatibility no-op | It cannot unmap safely and has no per-isolate state to release |
| N-API + raw V8 cast | Plain V8 addon | F3/F4: removes undocumented cast and dangling handle; N-API gave no ABI benefit here |
| `cleanupOnExit` | Attach-table refcount + `reap()` + `unlinkWhenUnused` | Exit hooks miss crashes; first-exit unlink splits processes onto different memory |
| `list()` by undefined "our prefix", Linux-only paths assumed everywhere | Magic-based `list`, explicit per-platform support matrix | Names are user-chosen; macOS/Windows have no `/dev/shm` |
| Fallback on by default, with warning | Fallback opt-in; Mutex/Ring refuse it | Silent single-process "success" is a production bug |
| "Zero runtime dependencies" | One runtime dep: `node-gyp-build` (no transitive deps) | F11: shmbuf was not dependency-free; the claim was false |
| Perf gate ≥ 5M / ~11M ops/s in tests | Benchmarks separate from tests; no throughput pass/fail in CI | F10: measurement too coarse, and shared CI runners are noisy |
| Node 18/20/22/24, win/mac smoke | Node 22/24/26, all three OSes fully tested | 18 and 20 are EOL; 26 is current (U4); win/mac behave most differently |
| Valgrind + ASAN | ASAN/UBSan only (LD_PRELOAD libasan into node) | Valgrind on V8's JIT is impractically slow and noisy |
| `tsc` dual CJS + ESM builds | CJS build + thin ESM wrapper | Two module copies would mean two JS-side registries in one process |

## 12. Layout & build

```
membridge/
  package.json  tsconfig.json  LICENSE  README.md  .gitignore  PLAN.md
  .github/workflows/ci.yml  .github/workflows/prebuild.yml
  src/    index.ts core.ts sync.ts mutex.ts ringbuffer.ts ops.ts errors.ts fallback.ts native.ts
  esm/    index.mjs  (re-exports the CJS build)
  cc/     addon.cc segment.cc registry.cc header.cc wait.cc liveness.cc *.h binding.gyp
  test/   *.test.ts + helpers.ts   (node:test — no test dependencies)
  bench/  contention.js mutex.js ring.js
```

- `engines: ">=22"`. **Why:** prebuilds are per ABI, so every supported major multiplies the build
  matrix; 18 and 20 are past EOL. Node 22 leaves maintenance in April 2027 (per Node's
  schedule), so within a year the matrix drops to two majors (24/26). Dropping 22 is a
  semver-major change: bump `engines` in a major release, not a minor.
- **README compatibility matrix** (written in M7): OS × arch × libc × Node major, plus runtime
  notes: official Node builds ✓ (no V8 sandbox, F13); **Electron ✗** (V8 sandbox rejects
  external backing stores); Bun/Deno untested; macOS wait latency depends on the U1 outcome;
  `list/capacity/reap()` Linux-only; Mutex has no priority inheritance (§7.3).
- Prebuilds (prebuildify, **per-ABI, not `--napi`**): linux-x64-glibc, linux-arm64-glibc,
  linux-x64-musl, darwin-arm64, darwin-x64, win32-x64 × Node 22/24/26.
  Loader: `node-gyp-build`. Source-build fallback via `node-gyp` (ships with npm; no `node-addon-api` needed).
- A new Node major needs a new release with fresh prebuilds — documented in README.

## 13. Tests (node:test)

**Runner contract:** tests are written as `*.test.ts`, compiled by the same
`tsconfig` into `dist/test/`, and run as `node --test dist/test/` — `npm test`
is `build` + that command. Source maps stay enabled so a failing crash test
reports TypeScript line numbers, not transpiled ones. No test dependencies
beyond the built-in `node:test`.

**`test/helpers.ts`** carries the machinery every crash test needs, so no suite
grows its own copy: the segment-name generator (`/membridge-test-<unique>`,
AGENTS.md guardrail 3), a cleanup harness that unlinks in `finally` even when a
test fails mid-way, a per-OS kill helper (POSIX `SIGKILL`; Windows `taskkill /F`
— timing differs, so kill-based assertions are state-based, never timing-based),
and an error-code assert (`assertThrowsCode(fn, 'E_SIZE_MISMATCH')`).

**Ported from shmbuf:** its 20 behaviors (SAB type, length, zero-init, all Atomics ops,
same-process re-open, isolation between names, lifecycle no-ops, fork handshake), and the
8-process lost-update check as a **correctness** test (no throughput threshold).

**New — core:** size validation (2^32+16, 1.9, −1, NaN, > cap); every create mode × size
policy; N-process join race on one name; creator SIGKILLed mid-init → joiner takes over;
`E_NO_SPACE` on a small dedicated tmpfs (CI mounts one); header magic/version mismatch;
worker_threads share one mapping; registry/GC lifecycle (with `--expose-gc`): mapping unmapped only
after the last SAB in every isolate is GC'd; `at-least` prefix + full view over one mapping;
after `grow`, a larger `open()` in the same process gets a new, larger SAB while old SABs stay valid;
`close()` is a no-op (SAB still usable after it, invalid name still throws).

**New — sync:** cross-process `wait`/`notify` wake latency (asserts a wake happens well before the
timeout — the F1 regression test), `waitAsync` does not consume libuv pool threads, and a
worker `.terminate()`d with a pending `waitAsync` has its promise resolved (`'timed-out'`)
and its `Mapping` reference dropped (no memory or `/dev/shm` leak).

**New — Mutex:** mutual exclusion across 8 processes + workers; holder SIGKILLed →
steal + `ownerDied`; worker terminated while holding → recovered via cleanup hook;
`E_NOT_OWNER`, `E_DEADLOCK`, timeouts, `lockAsync` with `AbortSignal`; PID-reuse simulation
(fake identity with wrong start time); slot-table exhaustion — 64 dead participants' slots
are reclaimed and a 65th thread can lock.

**New — RingBuffer:** cross-process stream with checksums across wrap/SKIP boundaries; full/empty
blocking; `E_MESSAGE_TOO_LARGE`; producer killed mid-reserve (nothing visible);
consumer killed mid-peek (message redelivered); role takeover after death; `E_ROLE_TAKEN`.

**New — ops:** `list/stat/reap` with live, dead and mixed attachers; `unlinkWhenUnused`.

**Platform rules:** the `E_NO_SPACE` tmpfs test runs only when `MEMBRIDGE_TEST_TMPFS`
points at a prepared small tmpfs and otherwise skips with that reason (the ubuntu CI
job mounts one); ring capacities above the default cap assert `E_SIZE_INVALID`
until the env override is raised.

**CI matrix:** {ubuntu, macos, windows} × Node {22, 24, 26}, full suite everywhere
(platform-specific tests skip with an explicit reason). Separate Linux ASAN/UBSan job.

## 14. Milestones (agentic rounds)

| # | Milestone | Exit criteria | Size |
|---|-----------|---------------|------|
| 1 | **Spike** | Plain V8 addon builds and wraps mmap (offset window, deleter unmaps base) on Node 22/24/26 (U4); in-process Atomics pairing across distinct BackingStores (U5); shared futex wakes across processes on Linux; macOS `os_sync` SHARED (U1) and shm name/grow limits (U2) and Windows named-semaphore wait (U3) checked on CI runners. Results written into §2. | 1–2 rounds |
| 2 | Native core | header, registry, size policies, reserve, names, errors, fallback; core tests green on all 3 OSes | 2 rounds |
| 3 | Sync primitive | `sync.wait/notify/waitAsync` on 3 OSes; waiter thread; F1 regression test | 1–2 rounds |
| 4 | Mutex | §7 complete with crash tests | 1–2 rounds |
| 5 | RingBuffer | §8 complete with crash tests | 1–2 rounds |
| 6 | Ops | `capacity/stat/list/reap/unlinkWhenUnused` | 1 round |
| 7 | Build & release pipeline | prebuild workflow, ESM wrapper, README/API docs, sanitizer job | 1–2 rounds |
| 8 | Full verification | full matrix green, benches recorded, `npm pack --dry-run` contents reviewed | 1 round |

Estimated total: **~10–14 agent rounds** (rev 1 said 6–9; the increase is the native wait
primitive, the header/attach table and real crash recovery — work rev 1 had left implicit).

## 15. Decisions you may want to override

- Author name in `package.json` / `LICENSE` is a placeholder ("Faiz").
- `engines >=22` (drops Node 18/20). Supporting them adds 2 ABIs × 6 targets of prebuilds.
- `reserve: true` by default (safety over lazy memory). Flip if sparse segments are the main use.
- Default size policy `exact`; max segment 256 MiB.
- Header page in every segment (breaks byte-compatibility with shmbuf; `raw: true` opts out).
- Steal never happens across PID namespaces (safe, but a crash there needs manual `reap`).
- RingBuffer delivery is at-least-once when a consumer crashes mid-message.
- GitHub repo URL / CI badges are left out until you create the remote.
