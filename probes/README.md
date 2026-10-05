# probes/

Re-runnable evidence scripts. Every fact in PLAN §2 (F1–F12) and every open
question (U1–U5) maps to a script here that prints its result, so facts can be
re-verified after Node/OS upgrades instead of trusted from memory.

## Convention

- One script per fact: `f1-cross-process-notify.js`, `u2-macos-shm-name.js`, …
- Each script prints a `PASS`/`FAIL` line plus the measured values, exits 0/1.
- Creates segments only under `/membridge-probe-<unique>` and always cleans up.
- Probes may use platform-specific APIs freely; skip with a clear message when
  run on the wrong OS.

| Fact | Probe | Status |
|------|-------|--------|
| F1 cross-process `Atomics.notify` cannot wake | `f1-cross-process-notify.js` | to add in M1 |
| F7 `fallocate` on tmpfs charges up front | `f7-fallocate-tmpfs.js` | to add in M2 |
| U1 macOS `os_sync` SHARED wake | `u1-macos-os-sync.js` | spike, macOS CI runner |
