# AGENTS.md — shm-bridge

Cross-process shared memory for Node.js: a `SharedArrayBuffer` over OS shared
memory (Linux/macOS/Windows), plus a crash-safe Mutex, a zero-copy RingBuffer,
and ops utilities. Plain-V8 native addon (`node.h`, no N-API) under a TypeScript
layer. **Status: published — `shm-bridge@0.2.0` on npm, Linux/macOS/Windows
all pass the full suite on CI (Linux gates releases). [PLAN.md](./PLAN.md) is
the spec and outranks this file on design questions.**

## Layout

```
shm-bridge/
├── AGENTS.md            # this file — agent context, commands, guardrails
├── PLAN.md              # the spec: design + reasoning; §11 is the change log
├── package.json         # published from CI on v* tags — see guardrails
├── binding.gyp          # stays at repo ROOT (node-gyp-build's source fallback runs node-gyp from the package root)
├── src/                 # TypeScript sources (CJS target). One file per concern.
├── esm/                 # thin ESM wrapper re-exporting the CJS build (single module registry per process)
├── cc/                  # C++ addon sources: one .cc per native concern + private headers
├── dist/                # tsc output (gitignored; built before publish)
├── prebuilds/           # per-ABI .node binaries (gitignored in repo; CI embeds them into the npm tarball)
├── test/                # node:test suites (*.test.ts) — zero test dependencies
├── bench/               # benchmarks — never used as pass/fail CI gates
├── probes/              # re-runnable evidence scripts behind PLAN §2 facts (F1–F12, U1–U5)
├── docs/
│   ├── README.md        # docs map + support matrix pointer
│   ├── adr/             # Architecture Decision Records — one file per lasting "Why"
│   ├── compat.md        # OS × Node × Electron support matrix
│   └── release.md       # prebuild + publish runbook (tag → Actions → npm, provenance)
└── .github/workflows/   # ci.yml (3 OS × Node 22/24/26), prebuild.yml
```

Nearest-file-wins: `cc/AGENTS.md` (C++ rules) and `test/AGENTS.md` may be added
later and take precedence for files in those directories.

## Commands

The contract agents may rely on; each is verified from its milestone onward.

| Task | Command | Available |
|------|---------|-----------|
| Build native addon | `npx node-gyp rebuild` | M2 ✓ |
| Build TypeScript | `npm run build` | M2 ✓ |
| Run all tests | `npm test` (= build + `node --test dist/test/*.test.js`) | M2 ✓ |
| Run one test file | `node --test dist/test/<name>.test.js` | M2 ✓ |
| Benchmarks | `npm run bench` | M7 ✓ |
| Re-run a fact probe | `node probes/<fact>.js` | M1 |

## Guardrails

1. **Publishing is tag-driven, never hand-run.** Pushing `vX.Y.Z` makes CI
   test, assemble the prebuilds and `npm publish --provenance` (owner's
   `NPM_TOKEN` secret). Agents never run `npm publish` and never bump versions
   or remove `private` on their own initiative — that is a release-commit
   decision by the owner.
2. **Never commit build output**: `prebuilds/`, `dist/`, `*.node` are gitignored.
3. **`/dev/shm` hygiene**: every test/probe segment name starts with
   `/shm-bridge-test-` or `/shm-bridge-probe-` plus a unique suffix, and is
   `unlink`ed in a `finally`/cleanup path. Segments survive crashes — leaked
   names are someone else's `/dev/shm` garbage.
4. **Waitable words are `i32` only** (futex/`os_sync`/semaphore constraint, §6).
   The user-facing SAB covers the data region only; the header page is
   native-only memory.
5. **Errors are `E_*` codes** on `ShmBridgeError`. A new code goes into PLAN §10
   and `src/errors.ts` in the same change.
6. **Platform truth lives in `docs/compat.md`**: Windows — no `grow`, no
   `capacity`/`list`; macOS — 31-char name limit if U2 holds, no `grow` (U2),
   no `capacity`/`list`; wait/notify mechanism differs per OS but the API doesn't.
7. **Design changes flow PLAN.md → code → ADR.** A code change that contradicts
   PLAN.md without a PLAN.md update is a bug even if the tests pass.
8. **Node support is 22/24/26** (`engines >=22`). A new Node major requires new
   prebuilds and a release — documented in `docs/release.md`.

## Facts are reproducible

Every claim in PLAN §2 came from a probe on the dev machine. Probes live in
`probes/` and must keep working; when a spike resolves a `U*` fact, its probe
lands here too and PLAN §2 is updated. Don't trust a fact whose probe is gone.
