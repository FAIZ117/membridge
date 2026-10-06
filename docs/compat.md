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
| `grow` size policy | ✓ (ftruncate under the init lock) | ✗ → `E_GROW_UNSUPPORTED` if U2 holds (shm objects cannot be re-`ftruncate`d) | ✗ → `E_GROW_UNSUPPORTED` (sections are fixed at `CreateFileMappingW` time) |
| Header page + attach table | ✓ | ✓ | ✓ |
| `sync.wait` / `notify` | ✓ verified: shared futex (F14: raw syscall, no `FUTEX_PRIVATE_FLAG`) | implemented: `os_sync` SHARED **with timeout**, weak-linked, else bounded poll (50 µs → 2 ms) — **CI-unverified (U1)** | implemented: named semaphores per word, chunked bounded releases — **CI-unverified (U3)** |
| `sync.waitAsync` | ✓ verified: `futex_waitv` multiplexer (kernel ≥ 5.16), else one thread per wait | implemented: one thread per wait over `os_sync_with_timeout`/poll — **CI-unverified (U1)** | implemented: one thread per wait over semaphores — **CI-unverified (U3)** |
| `Mutex` | ✓ | ✓ | ✓ |
| `RingBuffer` | ✓ | ✓ | ✓ |
| `capacity()` | ✓ (`statfs /dev/shm`) | ✗ `E_UNSUPPORTED` | ✗ `E_UNSUPPORTED` |
| `stat(name)` | ✓ | ✓ (native header read) | ✓ (read-only section view) |
| `list()` | ✓ (magic-filtered readdir) | ✗ `E_UNSUPPORTED` (POSIX shm cannot be enumerated) | ✗ `E_UNSUPPORTED` |
| `reap()` scan form | ✓ (needs `list`) | single-segment `reap(name)` only | single-segment `reap(name)` only |
| Segment names | `/x`, ≤ 250 B | `/x`, ≤ 31 B if U2 holds (`PSHMNAMLEN`) | escaped to `Local\membridge…` (`/`→`%2F`, `%`→`%25`); `Global\` opt-in needs `SeCreateGlobalPrivilege` |

**Correction (2026-10-06 fix round):** before this round this matrix marked the
macOS/Windows mechanism rows ✓ from source reading only — the code did not
even compile there (review F19) and the sync paths fell back to polling on
macOS. The rows above now say implemented/verified honestly; the first CI run
on those runners is the gate for flipping them to ✓.

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
| Node on macOS / Windows | source compiles per the platform guards; **the runners have not built it yet** — treat every non-Linux cell above as pending the first CI run (review F19 found the pre-fix tree could not compile there at all) |
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
