# Release runbook (owner-only)

shm-bridge publishes the same way as `expr-eval-nextgen`: **from GitHub Actions
with npm provenance** (`npm publish --provenance`, OIDC `id-token: write`,
`NPM_TOKEN` in repo Secrets). Agents never publish and never hold the token.
One addition is unavoidable because shm-bridge is a native addon: the 18
per-ABI prebuilds (6 targets × Node 22/24/26) cannot be produced on one dev
machine, so the release workflow builds them first and publishes the assembled
tarball from the same run.

## One-time, before the first public release

1. Create the GitHub repo `FAIZ117/shm-bridge` and add the remote:
   `git remote add origin https://github.com/FAIZ117/shm-bridge.git`
2. Put the npm automation token in repo Secrets as `NPM_TOKEN`
   (npmjs.com → Access Tokens → Generate New Token → type "Automation").
3. Remove `"private": true` from `package.json` in the version-bump commit
   (`expr-eval-nextgen` carries no `private` field; shm-bridge keeps it until
   the first real release as an accident guard — the publish job also refuses
   a still-private package).
4. Confirm `npm whoami` → `fhjami`.

## Per release

1. Update `CHANGELOG.md`, bump `version` — the version-bump commit is the
   release commit (`... (vX.Y.Z)`), matching the expr-eval-nextgen history style.
2. Tag `vX.Y.Z` and push the tag. `release.yml` then:
   - runs the full test matrix ({ubuntu, macos, windows} × Node 22/24/26),
   - builds all prebuilds via the reusable `prebuild.yml` and assembles the
     package exactly per the `files` whitelist
     (`dist/ esm/ prebuilds/ cc/ binding.gyp`), sanity-checking 18 prebuilds
     and that the tag matches `package.json`'s version,
   - and publishes **from Actions**: `npm publish --provenance --access public`
     (the npm page then shows the verified-provenance badge, as on
     `expr-eval-nextgen`). The assembled tarball stays available as the
     `shm-bridge-publish` artifact for the release notes.
3. Cut a GitHub release with the CHANGELOG section as notes.

### Fallback: manual publish

If Actions publishing must be avoided for one release, download the
`shm-bridge-publish` artifact and run `npm publish shm-bridge-x.y.z.tgz` locally
— that release just will not carry provenance.

## Rules

- **Never publish from a dev-machine build** — a Linux laptop produces a
  single-platform tarball; only the CI artifact carries all 18 prebuilds.
- If a platform's prebuild fails CI, ship the release **without** silently
  dropping it: either fix it or remove the platform from `docs/compat.md` for
  that version and say so in the CHANGELOG.
- New Node major (27+) ⇒ new ABI ⇒ new prebuild column and a release; noted in
  README per PLAN §12.
