# Release runbook (owner-only)

membridge publishes the same way as `expr-eval-nextgen`: **manual `npm publish`
by the owner** (npm account `fhjami`, GitHub org `FAIZ117`). Agents never
publish. One addition is unavoidable because membridge is a native addon: the
18 per-ABI prebuilds (6 targets × Node 22/24/26) cannot be produced on one dev
machine, so CI assembles the publish artifact and the owner publishes from it.

## One-time, before the first public release

1. Create the GitHub repo `FAIZ117/membridge` and add the remote:
   `git remote add origin https://github.com/FAIZ117/membridge.git`
2. Remove `"private": true` from `package.json` in the version-bump commit
   (`expr-eval-nextgen` carries no `private` field; membridge keeps it until
   the first real release as an accident guard).
3. Confirm `npm whoami` → `fhjami`.

## Per release

1. Update `CHANGELOG.md`, bump `version` — the version-bump commit is the
   release commit (`... (vX.Y.Z)`), matching the expr-eval-nextgen history style.
2. Tag `vX.Y.Z` and push the tag. `prebuild.yml` then:
   - runs the full test matrix ({ubuntu, macos, windows} × Node 22/24/26),
   - builds all prebuilds, assembles the package exactly per the `files`
     whitelist (`dist/ esm/ prebuilds/ cc/ binding.gyp`), and
   - uploads the ready-to-publish tarball as the `membridge-publish` artifact.
3. Download the artifact and publish it:
   `npm publish membridge-x.y.z.tgz`
4. Cut a GitHub release with the CHANGELOG section as notes.

## Rules

- **Never publish from a dev-machine build** — a Linux laptop produces a
  single-platform tarball; only the CI artifact carries all 18 prebuilds.
- If a platform's prebuild fails CI, ship the release **without** silently
  dropping it: either fix it or remove the platform from `docs/compat.md` for
  that version and say so in the CHANGELOG.
- New Node major (27+) ⇒ new ABI ⇒ new prebuild column and a release; noted in
  README per PLAN §12.
