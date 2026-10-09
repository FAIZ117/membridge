# Handoff — remaining experimental-platform issues (2026-10-09, late)

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

## REMAINING issues, in priority order

### R-B. Windows Node 22 — the cross-process ring/role cluster

| Test | Symptom | Working hypothesis |
|---|---|---|
| `round3.test.js` (whole file) | times out at 120 s | ONE of its tests hangs — run the file alone (`node --test dist/test/round3.test.js`) and bisect; prime suspect C4's role-claim settle loop (`cc/mutex.cc` ClaimRoleLocked → the P4-4 unique_lock settle) |
| C4 (91) | N racing workers, exactly one consumer | same settle loop, or the role-word CAS losing forever on Windows semaphore timing |
| 84 / 80 | consumer killed mid-peek / cross-process stream | the Windows waiter-thread path: `WaitThreadMain` `_WIN32` branch — check the semaphore permit bookkeeping after the round-1 kOk change (a permit consumed by a waiter that then loops without clearing may starve the next waiter) |
| 54 | 8-process join race | possibly cascading from 80/84 (shared fixture), else Windows `CreateFileMappingW` ALREADY_EXISTS race in OpenSegment |
| 33 (F22) | ownerDied reported once — `false !== true` | the stealer acquired WITHOUT the steal path: on Windows `MutexOwnerAlive` for the killed holder may report alive (OpenProcess succeeds on a terminated-but-referenced process and GetExitCodeProcess returns the exit code only after termination is FULLY complete — add a small retry or treat `GetExitCodeProcess != STILL_ACTIVE` with a second confirmation read) |
| 9 | size validation | the env-lift test (`core.test.ts` ~121) is only gated on MEMBRIDGE_TEST_TMPFS — on Windows CI it opens 256 MiB+1 for real; gate on `process.platform !== 'win32'` too, or verify the pagefile section actually maps |

Windows Node 24/26: whole-file hangs (25-min cap cancels them) — bisect with
the Node 22 file-alone approach once the cluster above is understood; the
worker-teardown suspicion (worker `.terminate()` with native waiter threads)
is documented in compat.md.

### R-C. macOS — remaining items after R-A

## Release mechanics (unchanged)

- Publish fires on `v*` tags: test-linux × 3 → prebuilds (6 targets × 3 ABIs,
  sanity-checked 18) → `npm publish --provenance` from Actions with
  `NPM_TOKEN` (set). Experimental platforms run in `test-experimental`,
  non-blocking.
- The npm name is **shm-bridge** (`membridge` and `memfuse` were both 403'd
  by the typosquat rule — vs `mem-bridge` and `memfs`). Internal codenames
  (MembridgeError, `MEMBRIDGE_*` env, `/membridge-*` segment prefixes, the
  repo's PLAN.md) intentionally stay "membridge".
- v0.1.0 is published; the NEXT release needs a version bump (0.1.1) + the
  private-guard is already down. Re-pointing `v0.1.0` re-runs the pipeline
  but npm rejects re-publishing an existing version (E409) — expected.
