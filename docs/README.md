# membridge — docs

Documentation for humans and agents. The normative design spec is
[PLAN.md](../PLAN.md) at the repo root; this directory holds the documents that
outlive individual plan revisions.

| File | What it is |
|------|------------|
| `adr/` | Architecture Decision Records — one numbered file per lasting design decision, with alternatives considered |
| `compat.md` | Support matrix: OS × feature × Node version × Electron caveat |
| `release.md` | Runbook for prebuilds and `npm publish` (owner-only step) |

## ADRs

Numbered `NNNN-short-title.md`, immutable once accepted (supersede instead of
edit). Start from [`adr/0000-adr-template.md`](./adr/0000-adr-template.md).

Seeded when milestones land, e.g.: native wait/notify instead of
`Atomics.wait` (M3), single-word CAS mutex without heartbeat (M4), header page
in every segment (M2). Until then the reasoning lives in PLAN.md §5–§9 and §11.
