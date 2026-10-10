# Handoff — remaining experimental-platform issues (2026-10-09, late)

> **RESOLVED 2026-10-10** — everything below was fixed the same day; see
> CHANGELOG 0.2.0 and compat.md. Windows went green on CI (22/24/26, run
> `37977949293`: 91 pass / 0 fail / 32 POSIX-only skips); the root was test-
> side section lifetime, not the library. Kept as the historical record of
> the debugging trail.

| | |
|---|---|
| State at handoff | `9ca9c50` (main; tag `v0.1.0` points here). **shm-bridge@0.1.0 is LIVE on npm** with verified SLSA provenance and 18 per-ABI prebuilds. |
| In flight | CI `37974031646` finished: **macOS × 22/24/26 GREEN**, Linux green, Windows Node 22 red (R-B below), Windows 24/26 cancelled at the 25-min cap. |
| Verified working | Linux (full matrix 22/24/26 + ASAN/UBSan + small-tmpfs + pack-and-install smoke), npm publish pipeline end-to-end (provenance signed, transparency log). |
| Not verifiable locally | macOS and Windows — every item below is debugged through ~25-minute CI rounds. A local macOS (14.4+) and Windows box would compress this work enormously. |
| Hygiene at handoff | `/dev/shm` clean, git tree clean, no secrets in the repo. `NPM_TOKEN` set on the repo (write-only). |

## How to run the verification loop

```sh
git push origin main                      # CI fires (Linux gates + experimental legs)
git tag -f v0.1.0 <sha> && git push --force origin v0.1.0   # re-fires Release (re-publishes if version bumped)
gh run watch <id> --repo FAIZ117/shm-bridge --exit-status
gh run view <id> --repo FAIZ117/shm-bridge --log-failed > failed.log   # only after the run completes
gh api repos/FAIZ117/shm-bridge/actions/jobs/<job-id>/logs --allow-escape-sequences   # per-job, works mid-run once the job finished
```

Gotchas learned the hard way: `gh run --log-failed` refuses while the run is
in progress; job logs are fetchable individually once THAT job finished; the
20-minute `timeout-minutes` on experimental jobs concludes as **"cancelled"**,
which `continue-on-error` does NOT cover — that is why release.yml's publish
depends only on `test-linux` + `prebuilds`.

## Fixed this session (rounds 1–4, all committed on main)

1. **Cross-process wake semantics (macOS + Windows)** — a store-less
   `sync.notify` could never resolve `'ok'`: Linux's futex wake returns
   r==0 → `'ok'` regardless of the value, but the macOS `os_sync` and Windows
   semaphore paths re-checked the value first. `OsSyncPark` now returns
   Woken/Parked/Unavailable; a wake lands as `'ok'` with or without a value
   change; a value that already differs before any park is `'not-equal'`
   (§6 parity with futex EAGAIN). **Verified fixed on CI** (F1/117–120 gone
   from both platforms).
2. **Windows kind/size-mismatch opens** — an existing section opened with a
   different size was mapped with the REQUESTED size (kernel refuses a view
   larger than the section → ACCESS_DENIED → misleading E_SYSTEM). Existing
   sections now map WHOLE (size 0) and the header checks throw the real
   error. (F33 fixed on CI.)
3. **macOS `shm_open` descriptors do not service read()/pread() at all** —
   both return EOF on a fully sized object (the diagnostic that cracked it:
   "0 bytes read, object size 32768"). `ReadHeader` now reads the header
   through an **mmap**. (66/67 expected fixed — verify in the in-flight run.)
4. **darwin liveness** — self-shortcut now compares the row's start time
   (a stale row with our pid + foreign start is DEAD — PID-reuse test 62
   fixed on CI); a zombie's `proc_pidinfo` returns **0 bytes** (no BSD info
   on a corpse) → that IS the dead verdict; a short fill keeps pbi_status
   (zombie detection) and reports start −1 → kUnknown = never steal.

## RESOLVED (was R-A) — macOS is GREEN

**Verified 2026-10-10, run `37974031646`: macOS × Node 22/24/26 all pass the
full suite.** The fixes that closed it: the mmap ReadHeader (shm descriptors
don't service read/pread), the zero-info zombie verdict, the self-start
compare in CheckLiveness, and the wake-semantics repair. No macOS items
remain; keep the leg watched for flake regressions only.

## RESOLVED (was R-B) — Windows is GREEN

**Verified 2026-10-10, run `37977949293`: Windows × Node 22/24/26 all pass
the full suite (91 pass, 0 fail, 32 POSIX-only skips).** The R-B hypotheses
above were wrong; the real causes:

| Test | Real cause | Fix |
|---|---|---|
| 54, 80, 84, 91 (C4), 120 | the creator's discarded `open()` SAB was GC'd; a Windows section dies with its last handle, so joiners saw `E_NOT_FOUND` / a fresh zeroed object | `holdForTest(t, …)` in test/helpers.ts at every creator a later joiner depends on (`5b799aa`/`08bad9b`) |
| 33 (F22) | same lifetime issue: the dead child held the only handle, so the "steal" created a fresh mutex (`ownerDied` false) | parent attaches before the holder dies |
| `round3.test.js` 120 s / Node 24/26 hangs | C4's failed assert skipped the worker exit handshake; live workers pinned the loop | C2/C4 terminate workers in `finally` |
| 9 | native cap read the CRT `getenv`, which never sees runtime `process.env` sets on Windows | env read via `uv_os_getenv` (`cc/addon.cc`) |
| 122 / 118 (found after the above) | a change seen after a park returned `'not-equal'`; an async wait's semaphore was created only once its thread ran, dropping an earlier store-less notify | `parked` tracking in both Windows wait paths; semaphore opened in StartAsyncWait (`49f9fca`) |
| R1 | was skipped on Windows for the same misdiagnosed reason | un-skipped; passes |

Watch only: Perf P4 (`lock({timeoutMs:10})` took 212 ms once on Node 24,
passed on the rerun) — runner scheduling jitter, not reproduced.

## Release mechanics (unchanged)

- Publish fires on `v*` tags: test-linux × 3 → prebuilds (6 targets × 3 ABIs,
  sanity-checked 18) → `npm publish --provenance` from Actions with
  `NPM_TOKEN` (set). Experimental platforms run in `test-experimental`,
  non-blocking.
- The npm name is **shm-bridge** (the original codename and `memfuse` were
  both 403'd by npm's typosquat rule). Since 0.2.0 everything — repo, error
  class (`ShmBridgeError`), `SHM_BRIDGE_*` env, `/shm-bridge-*` segment
  prefixes, Windows object names, the header magic ("SHMB") — carries the
  shm-bridge name.
- v0.1.0 is published; 0.2.0 is the next release (the rename is breaking).
  Re-pointing an existing tag re-runs the pipeline but npm rejects
  re-publishing an existing version (E409) — expected.
