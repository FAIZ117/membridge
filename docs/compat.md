# Platform support matrix

Platform truth lives here (AGENTS.md guardrail 6). Everything not marked
otherwise is verified by the CI matrix ({ubuntu, macos, windows} × Node
22/24/26). "M-dev" rows were verified on the dev machine (Linux) and by CI
after the owner pushes.

## OS × feature

| Feature | Linux | macOS | Windows |
|---------|-------|-------|---------|
| `open` / join / sizes / policies | ✓ | ✓ | ✓ |
| `reserve` (fallocate, E_NO_SPACE) | ✓ | ✗ (no-op; fallocate has no shm meaning there)¹ | ✗ (no-op)¹ |
| `grow` size policy | ✓ (ftruncate under the init lock) | ✗ → `E_GROW_UNSUPPORTED` (U2: shm objects cannot be re-`ftruncate`d; refused up front since round 3) | ✗ → `E_GROW_UNSUPPORTED` (sections are fixed at `CreateFileMappingW` time) |
| Header page + attach table | ✓ | ✓ | ✓ |
| `sync.wait` / `notify` | ✓ verified: shared futex (F14: raw syscall, no `FUTEX_PRIVATE_FLAG`) | implemented (round 3 C7): `os_sync_wait_on_address_with_timeout` (`OS_CLOCK_MACH_ABSOLUTE_TIME`, slices ≤ 250 ms with word/deadline re-check) and `os_sync_wake_by_address_all` (`OS_SYNC_WAKE_BY_ADDRESS_SHARED`), runtime-guarded with `__builtin_available(macOS 14.4)`; older SDK/OS → bounded poll (50 µs → 2 ms) — **CI-unverified (U1)** | implemented: named semaphores per word, chunked bounded releases — **CI-unverified (U3)** |
| `sync.waitAsync` | ✓ verified: `futex_waitv` multiplexer (kernel ≥ 5.16), else one thread per wait | implemented (round 3 C7): wait thread over timed `os_sync` slices / poll — **CI-unverified (U1)** | implemented: one thread per wait over semaphores — **CI-unverified (U3)** |
| `Mutex` | ✓ | ✓ | ✓ |
| `RingBuffer` | ✓ | ✓ | ✓ |
| `capacity()` | ✓ (`statfs /dev/shm`) | ✗ `E_UNSUPPORTED` | ✗ `E_UNSUPPORTED` |
| `stat(name)` | ✓ | ✓ (native header read) | ✓ (read-only section view) |
| `list()` | ✓ (magic-filtered readdir) | ✗ `E_UNSUPPORTED` (POSIX shm cannot be enumerated) | ✗ `E_UNSUPPORTED` |
| `reap()` scan form | ✓ (needs `list`) | single-segment `reap(name)` only | single-segment `reap(name)` only |
| Segment names | `/x`, ≤ 250 B | `/x`, ≤ 31 B if U2 holds (`PSHMNAMLEN`) | escaped to `Local\membridge…` (`/`→`%2F`, `%`→`%25`); **case-insensitive** — `/Foo` and `/foo` are the same Windows section (review F8); `Global\` opt-in needs `SeCreateGlobalPrivilege` |

**Known issues on non-Linux (updated 2026-10-10):**

- **macOS: GREEN.** Node 22/24/26 pass the full suite on the runners
  (2026-10-10, run `37974031646`). The failures found and fixed en route:
  a store-less notify never resolved 'ok' (the os_sync wake return was
  ignored — woken is 'ok' with or without a value change), shm descriptors
  do not service read()/pread() (ReadHeader now reads through an mmap), a
  zombie's proc_pidinfo returns zero bytes (that IS the dead verdict), and
  a stale row carrying our pid with a foreign start is dead, not alive.
- **Windows: GREEN.** Node 22/24/26 pass the full suite on the runners
  (2026-10-10, run `37977949293`: 91 pass, 0 fail, 32 POSIX-only skips).
  The root of the cross-process cluster and the Node 24/26 hangs was test
  lifetime, not the library: a named section dies with its last handle
  (§5.4), so a creator whose SAB was garbage-collected took the object with
  it before children/workers joined (they saw `E_NOT_FOUND` or a fresh
  zeroed section; a failed assert then left workers pinning the event
  loop). **Users: on Windows, keep the creator's SAB (or Mutex/ring handle)
  reachable for as long as the segment must exist** — POSIX names persist
  until `unlink`, Windows names do not. Library fixes found en route: a
  change seen after a park is `'ok'` (not `'not-equal'`), `waitAsync`
  registers before returning (no lost store-less wake), and
  `MEMBRIDGE_MAX_SEGMENT_BYTES` set via `process.env` at runtime is honoured
  natively (read through libuv, not the CRT's startup copy).
- Fixed en route during these rounds: Windows compile (`SEMAPHORE_MODIFY_STATE`),
  the Linux-only S_ISREG guard (darwin shm fdstat is not S_IFREG), Windows
  `stat()` liveness losing FILETIME precision through a JS double, darwin
  31-byte name compression in the test harness, whole-section mapping for
  existing sections of a different size.

**Correction (round 3, 2026-10-07):** the round-2 text below claimed "untimed
parks in bounded slices" — an untimed `os_sync` park is not bounded, so on macOS
lock timeouts, the 250 ms dead-owner slice and initializer-death checks never
fired without a wake (C7). The SDK header was also included inside an anonymous
namespace. Both fixed; still **CI-unverified**. macOS liveness now matches Linux
(`proc_pidinfo` unreadable → unknown, `SZOMB` → dead), and Windows `stat()`/
`reap()` use the native OpenProcess check instead of always reporting
`'unknown'` (F34). Windows `stat()` bounds its row loop by the mapped view (C8).

**Correction (2026-10-06 fix round 2):** the macOS sync paths no longer fake
weak-import declarations of `os_sync` symbols that may not exist (R17) — they
compile against the real SDK headers, gated on `__MAC_14_4`, and use
`os_sync_wake_by_address_all` (the only one of the three wake exports that
actually ships). Windows join now derives its mapping extent from `VirtualQuery`
instead of assuming zero (R16). Both remain **CI-unverified** until the first
runner build passes; do not ship non-Linux claims before that.

¹ The option is accepted everywhere; on non-Linux it does nothing (lazy tmpfs
charge is a Linux tmpfs property). `docs` say so; the create still cannot
SIGBUS-at-a-distance there because macOS/Windows charge differently.

² Wake mechanism differs per OS but the API does not (§6). Missed wakes cost
bounded latency, never correctness: every waiter re-checks the word on wake
and on timeout.

## Runtimes

| Runtime | Status |
|---------|--------|
| Node 22 / 24 / 26 (official builds, Linux) | ✓ verified locally on every fix round |
| Node on macOS / Windows | **compiles and runs on CI (2026-10-08)**; mid-integration: CI runs these legs **non-blocking** while the issues below are fixed. Linux is the verified platform and gates the run |
| Electron | ✗ — the V8 sandbox rejects external backing stores (F13); unsupported by design |
| Bun / Deno | untested — likely broken (plain-V8 addon, node.h ABI) |
| worker_threads | ✓ — context-aware `NODE_MODULE_INIT`, process-wide native registry |

## Notes that outlive versions

- **Waitable words are `i32` only** — futex/`os_sync`/semaphore constraint
  (PLAN §6). The layouts use free-running `i32` counters instead of `u32`.
- **PID-namespace recovery**: a lock/ring role whose holder lives in a foreign
  pid namespace is treated as alive forever (`liveness: unknown → never
  steal`, §7.1); recovery needs a process in the owner's namespace or `reap`.
- **RingBuffer delivery is at-least-once** when a consumer dies mid-message
  (§8.2).
- **Mutex has no priority inheritance and no fairness guarantee** (§7.3) — do
  not use it for latency-critical control loops.
- `MEMBRIDGE_MAX_SEGMENT_BYTES` (default 256 MiB) caps segment sizes; a ring's
  capacity counts against it (`E_SIZE_INVALID` above the cap, §8.1).
- **`close()` releases a handle**: `Mutex`, `RingProducer` and `RingConsumer`
  instances are also released when garbage-collected, but call `close()` for
  deterministic hand-over — a second live consumer on one thread is
  `E_ROLE_TAKEN` until the first is closed or collected. `RingProducer.open` on
  a thread that already has a live producer for the same mapping returns that
  instance. Calls on a closed instance throw `E_CLOSED`.
- **Contended mutex profile** (ADR 0005): the always-OR wake protocol costs
  ≈1 FUTEX_WAKE per contended unlock and ~14% throughput at zero think time,
  in exchange for no slice-bound handoffs; newcomers can still barge (no
  fairness, §7.3), so tail waits of tens of ms under saturation are expected.
- **Full-ring wakes**: while a peer is parked on a full/empty ring, each
  release/commit notifies until the peer runs (a plain-load flag check — an
  exchange-based "one wake per park" loses wakes, ADR 0006).
- **Async waits are budgeted per process**: on Linux ≥ 5.16, 127 multiplexed +
  64 thread fallbacks = 191 outstanding `waitAsync`/`lockAsync`/ring async
  waits; on macOS, Windows and Linux < 5.16 only the 64 thread fallbacks. Then
  `E_TOO_MANY_WAITERS` (all segments and isolates share the budget).
- **Containers sharing `/dev/shm`** (`--ipc=…`) are supported with one rule:
  nothing is ever stolen or recovered across pid namespaces — a holder, or a
  claim in flight, that dies in another container stays held until a process
  in that namespace (or `reap`) recovers it (PLAN §7.1/§7.2).
- **`/dev/shm` is world-writable**: `list()`/`stat()`/`reap()` skip FIFOs,
  devices and symlinks other users plant there (no hang, no abort); opening
  such a name fails `E_INCOMPATIBLE`.
- `reap()` is for segments expected idle: it decides from an attach-table
  snapshot, so a joiner claiming a row mid-scan can be unlinked under (it
  keeps its mapping, POSIX-style). The same benign race exists in the last
  `Detach`'s unlink decision: a joiner that claims its row between the
  snapshot and the `shm_unlink` keeps a valid mapping of a now-unlinked
  object (review R24).
- **`raw: true` grow is not serialized across processes** (review F31): raw
  segments have no init lock, so two raw growers both `fstat` the old size and
  the smaller `ftruncate` can land last — shrinking the file under the other
  grower's mapping. Do not grow raw segments from multiple processes; use a
  header segment (its grow takes the init lock).
- **Mutex participant slots are bounded (64 per segment)**: when all 64 are
  held by live threads, `claim()` throws `E_TIMEOUT` (review F37). Exited
  workers free their slots via cleanup hooks; SIGKILLed processes' slots are
  reclaimed by the §7.1 liveness scan.
