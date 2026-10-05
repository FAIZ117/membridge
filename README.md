# membridge

Cross-process shared memory for Node.js — a `SharedArrayBuffer` backed by OS
shared memory (`shm_open`/`mmap` on Linux/macOS, `CreateFileMapping` on Windows),
shareable across processes and usable with `Atomics`. Ships a crash-safe
cross-process Mutex, a zero-copy RingBuffer, and ops utilities.

Supersedes [`shmbuf`](https://www.npmjs.com/package/shmbuf) (MIT, kedemd) —
same core technique, hardened: no SIGBUS on size mismatch, typed errors,
deterministic lifecycle, configurable limits, plus sync primitives.

> **Status: planning.** Nothing is implemented yet. The full implementation
> plan lives in [PLAN.md](./PLAN.md).

## Compatibility (planned)

| Runtime | Status |
|---------|--------|
| Node.js ≥ 22, official builds (Linux, macOS, Windows) | Supported target |
| Electron | **Not supported** — Electron enables the V8 sandbox, which rejects the external backing stores membridge is built on |
| Bun, Deno | Untested |

The Mutex has no priority inheritance and no fairness guarantee — don't use it for
latency-critical control loops. The full matrix (OS × arch × libc × Node major) is
filled in at release; see [PLAN.md](./PLAN.md) §12.

## License

MIT
