# probes/

Re-runnable evidence scripts. Every fact in PLAN §2 (F1–F17) and every open
question (U1–U3) maps to a script here that prints its result, so facts can be
re-verified after Node/OS upgrades instead of trusted from memory.

## Convention

- One script per fact: `f1-cross-process-notify.js`, `u5-atomics-pairing.js`, …
- Each script prints a `PASS`/`FAIL` line plus the measured values, exits 0/1.
- Creates segments only under `/shm-bridge-probe-<unique>` and always cleans up.
- Probes may use platform-specific APIs freely; skip with a clear message when
  run on the wrong OS.

## Build prerequisite

Probes drive the M1 spike addon (`spike/build/Release/shm_bridge_spike.node`),
which is ABI-bound (plain V8, not N-API). `probes/lib.js` rebuilds it
automatically for the running Node via `npx node-gyp rebuild
--target=<running version>` (headers come from the local cache or nodejs.org);
set `SHM_BRIDGE_SPIKE_ADDON` to point at a different build.

| Fact | Probe | Status |
|------|-------|--------|
| F1 cross-process `Atomics.notify` cannot wake | `f1-cross-process-notify.js` | PASS — M1, Linux, Node 22/24/26 |
| F2/F15 SAB over mmap via BackingStore; SAB length == store length (window at the BackingStore level) | `f2-sab-window.js` | PASS — M1 (`--expose-gc` enables the GC/deleter part) |
| F14 shared futex via raw syscall; `futex_waitv` support + return semantics | `f14-shared-futex.js` | PASS — M1, kernel 7.2.8: shared `FUTEX_WAIT`/`FUTEX_WAKE` wakes across processes; `futex_waitv` returns the **index** of the woken entry |
| U4 Node 26 current; `NewBackingStore` unchanged | run any probe on Node 26.10.0 | verified locally M1 (only adaptation: F16, `Context::GetIsolate()` removed in V8 14.6) |
| U5 `Atomics` pairing across distinct BackingStores over one mapping | `u5-atomics-pairing.js` | PASS — M1: V8 keys waiters by address; §5.4 per-open BackingStore design stands |
| F16 V8 14.6 (Node 26) removed `Context::GetIsolate()` | the spike addon itself (`spike/addon.cc` uses `Isolate::GetCurrent()`); re-verify via a 26.10.0 build | PASS — M1, build + full spike on 26.10.0 |
| F17 `Worker.terminate()` resolves promptly while parked on a custom-BackingStore SAB | `spike/u5-terminate.js` | PASS — M1, ~3 ms on Node 24.18/26.10 |
| U1 macOS `os_sync` SHARED wake | `u1-macos-os-sync.js` | open — needs a macOS runner; §6 polling fallback implemented in M3 regardless |
| U2 macOS shm name/ftruncate limits | `u2-macos-shm-name.js` | open — needs a macOS runner |
| U3 Windows `WaitOnAddress` process-private | `u3-windows-waitonaddress.js` | open — needs a Windows runner; §6 named-semaphore path implemented in M3 regardless |
| F7 `fallocate` on tmpfs charges up front | `f7-fallocate-tmpfs.js` | to add in M2 |
