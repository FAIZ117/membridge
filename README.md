# membridge

Cross-process shared memory for Node.js — a `SharedArrayBuffer` backed by OS
shared memory (`shm_open`/`mmap` on Linux/macOS, `CreateFileMapping` on Windows),
shareable across processes and usable with `Atomics`. Ships cross-process
wait/notify (`membridge/sync`), a crash-safe Mutex, a zero-copy RingBuffer,
and ops utilities.

Supersedes [`shmbuf`](https://www.npmjs.com/package/shmbuf) (MIT, kedemd) —
same core technique, hardened: no SIGBUS on size mismatch, typed errors,
deterministic lifecycle, cross-process synchronization, configurable limits.

## Install

```
npm install membridge
```

Prebuilds ship for Linux (x64/arm64 glibc, x64 musl), macOS (arm64/x64) and
Windows (x64), per Node ABI (22/24/26). A source build via `node-gyp` is the
fallback (needs a C++20 toolchain). The loader is
[`node-gyp-build`](https://www.npmjs.com/package/node-gyp-build); the only
runtime dependency.

## The one-paragraph mental model

Every membridge segment is a named OS shared-memory object with a native
header page (metadata, attach table, init protocol) and a data region. `open()`
hands you a `SharedArrayBuffer` covering **the data region only** — user
offsets start at 0. The header is native-only memory; you cannot corrupt it
from JS. The mapping lives while any SAB references it; `unlink()` removes the
name (POSIX semantics). `close()` is a compatibility no-op (PLAN §5.4).

## API

### `membridge` (core segments)

```ts
open(name: string, size: number, opts?: OpenOptions): SharedArrayBuffer
open(name: string, opts?: OpenOptions & { size?: undefined }): SharedArrayBuffer // join at existing size
close(name: string): void   // documented no-op (validates the name)
unlink(name: string): void
isNative(): boolean

interface OpenOptions {
  mode?: 'create-or-join' | 'create' | 'join';  // default create-or-join
  sizePolicy?: 'exact' | 'at-least' | 'grow';   // default exact
  reserve?: boolean;        // Linux: fallocate at create (E_NO_SPACE when full). default true
  permissions?: number;     // POSIX mode bits. default 0o600
  initTimeoutMs?: number;   // joiner wait for the creator. default 5000
  raw?: boolean;            // no header: foreign-process interop
  winGlobal?: boolean;      // Windows Global\ prefix (needs SeCreateGlobalPrivilege)
  kind?: 'plain' | 'mutex' | 'ring'; // header kind marker
  unlinkWhenUnused?: boolean; // the last detacher unlinks the name
  allowFallback?: boolean;  // opt into the in-process fallback (no native addon)
}
```

- Sizes are validated as safe integers (≥ 1, ≤ `MEMBRIDGE_MAX_SEGMENT_BYTES`,
  default 256 MiB). `exact` mismatches throw `E_SIZE_MISMATCH`; `at-least`
  maps a prefix; `grow` extends the segment POSIX-only (Windows:
  `E_GROW_UNSUPPORTED`).
- Creation/join races are handled by the header's init protocol: a joiner that
  arrives while the creator is initializing waits (bounded by
  `initTimeoutMs`); if the creator died, the joiner takes over.
- Without the native addon, `open` throws `E_NATIVE_UNAVAILABLE` unless you
  opt into the fallback (`MEMBRIDGE_ALLOW_FALLBACK=1` or `allowFallback:
  true`). The fallback is **this process only** — using it as if it were
  shared memory is a production bug waiting to happen.

### `membridge/sync` — cross-process wait/notify

`Atomics.notify` cannot wake a process that isn't ours (verified: PLAN §2
F1). This is the same API shape, implemented over OS primitives:

```ts
import { wait, waitAsync, notify } from 'membridge/sync';

wait(view: Int32Array, index: number, expected: number, timeoutMs?: number):
  'ok' | 'not-equal' | 'timed-out';   // blocks the calling thread like Atomics.wait
waitAsync(view, index, expected, timeoutMs?): Promise<'ok' | 'not-equal' | 'timed-out'>;
notify(view: Int32Array, index: number, count?: number): void;
```

Waitable words are **`Int32Array` elements only** (futex/`os_sync`/semaphore
constraint). Async waits run on membridge-owned threads, never the libuv
threadpool; on Linux ≥ 5.16 one multiplexer thread parks in `futex_waitv` for
up to 127 waits. Isolate teardown (worker exit or `.terminate()`) cancels that
isolate's pending waits and resolves them `'timed-out'`.

### `membridge` — Mutex (crash-safe)

```ts
import { Mutex } from 'membridge';

const mutex = Mutex.open('/my-mutex');
const { ownerDied } = mutex.lock({ timeoutMs: 1000 });
try {
  // ... protected section; if ownerDied, the previous holder died holding
  // the lock and the data may be half-updated — only you can repair it.
} finally {
  mutex.unlock(); // E_NOT_OWNER if we don't hold it
}

mutex.tryLock();                    // { ownerDied } or null when held (does not steal)
await mutex.lockAsync({ signal });  // AbortSignal-aware, non-blocking waits
mutex.withLock(({ ownerDied }) => { ... });
await mutex.withLockAsync(fn, { timeoutMs: 1000 });
mutex.close();                      // unlocks if held; later calls throw E_CLOSED
Mutex.unlink('/my-mutex');
```

- Single 32-bit token word: lock, unlock, and stealing a dead holder's lock
  are each one CAS. **No heartbeat** — a holder stuck in synchronous code
  cannot be falsely stolen, and a SIGKILLed holder is recovered on the next
  contender's liveness check (its first contended pass; at worst one 250 ms
  wait slice later).
- Each `Mutex` instance is an independent handle: closing or dropping one
  never releases a lock held through another instance (instances on one
  thread share the thread's participant slot, so `lock()` on a second
  instance while the first holds is `E_DEADLOCK`).
- Re-entrant locking throws `E_DEADLOCK`. Up to 64 live participant threads;
  slots of dead participants are reclaimed automatically.
- No priority inheritance, no fairness guarantee — don't use it for
  latency-critical control loops.

### `membridge` — RingBuffer (SPSC, zero-copy)

```ts
import { RingProducer, RingConsumer } from 'membridge';

// one producer, one consumer, capacity 4 KiB .. 1 GiB (power of two)
const p = RingProducer.open('/my-ring', { capacity: 65536 });
const view = p.reserve(n);            // Uint8Array straight into shared memory
view.set(bytes);
p.commit();                           // publishes: length store + head release
p.write(bytes);                       // convenience: reserve + copy + commit

const c = RingConsumer.open('/my-ring');
const msg = c.peek({ timeoutMs: 1000 }); // view into shared memory, or null
consume(msg);
c.release();                          // only now does the producer reclaim
const copy = c.read();                // convenience: peek + copy + release
c.close(); p.close();                 // release the roles; later calls throw E_CLOSED
```

- Framing: `u32 length + payload`, 8-byte aligned; a message that would
  straddle the end is preceded by a SKIP marker and wraps to offset 0.
  `maxMessage ≤ capacity/2 − 8` guarantees a wrap can never deadlock.
- Crash safety by construction: nothing is visible before `commit()`; a
  consumer that dies mid-`peek` leaves the message for the next consumer
  (at-least-once delivery).
- SPSC enforced with role claims: another thread or process opening a role
  that a live participant holds gets `E_ROLE_TAKEN`; after the holder dies (or
  closes / is garbage-collected), the next one takes over. On the same thread,
  `RingProducer.open` returns the producer already open for that ring, and a
  second consumer is `E_ROLE_TAKEN` until the first is closed.
- Timeouts: `0` means non-blocking; otherwise a finite number of ms up to
  2^31 (`E_NAME_INVALID` for NaN, negative or larger values).

### `membridge` — ops

```ts
import { capacity, stat, list, reap } from 'membridge';

capacity();          // { totalBytes, freeBytes } of the shm store (Linux only)
stat('/name');       // kind, sizes, attach table + liveness
list();              // membridge segments in /dev/shm (Linux only; magic-filtered)
reap({ dryRun });    // unlink segments with no live attachers (overflow-safe)
reap({ name });      // single-segment reap, every platform
open(name, n, { unlinkWhenUnused: true }); // last detacher unlinks
```

## Errors

Everything throws `MembridgeError` with a `code` (`E_SIZE_MISMATCH`,
`E_ROLE_TAKEN`, `E_DEADLOCK`, `E_NO_SPACE`, `E_TOO_MANY_WAITERS`, `E_CLOSED`, …) and
structured fields (`segmentName`, `requested`, `existing`, `syscall`,
`errno`). The full list lives in [PLAN.md §10](./PLAN.md).

## Compatibility

| Runtime | Status |
|---------|--------|
| Node.js 22 / 24 / 26, official builds | ✓ (Linux x64/arm64 glibc, Linux x64 musl, macOS arm64/x64, Windows x64) |
| Electron | **Not supported** — the V8 sandbox rejects external backing stores |
| Bun / Deno | untested |
| worker_threads | ✓ |

Feature matrix and platform notes: [docs/compat.md](./docs/compat.md).
Prebuild + publish runbook: [docs/release.md](./docs/release.md).

## Development

```
npm install        # builds the addon (node-gyp-build)
npm test           # tsc + node --test
npm run bench      # benchmarks (never a CI gate)
node probes/f14-shared-futex.js   # re-run a PLAN §2 fact probe
```

The design spec, with the reasoning behind every decision, is
[PLAN.md](./PLAN.md); architecture decision records live in
[docs/adr/](./docs/adr/).

## License

MIT — see [LICENSE](./LICENSE). Started from the lessons of
[shmbuf](https://www.npmjs.com/package/shmbuf) (MIT, kedemd).
