# 0001. Header page in every segment

- **Status:** accepted
- **Date:** 2026-10-06
- **Deciders:** Faiz (per PLAN rev 2)

## Context

shmbuf stored no metadata in its segments. Four problems followed: a joiner
that arrived before the creator sized the object read `st_size == 0` and
failed (or worse, mapped and SIGBUSed); the size check was `fstat`-based and
therefore POSIX-only (Windows sections have no size-readable stat); nobody
knew who was attached, so `list`/`stat`/`reap` were guesses; and a creator
that died mid-init left a poisoned segment (M1 spike: the zero-length object
crashes the first reader unless handled).

Evidence: PLAN §2 F6 (map-past-EOF), F7 (lazy tmpfs charge), and the M2
crash-state tests that craft headers by hand.

## Decision

Every membridge segment starts with a one-page header (max(4096, page size),
page-aligned) containing magic + layout version, an init state machine
(uninit → initializing → ready), authoritative sizes, kind flags, and a
32-byte-per-row attach table (~126 rows at 4 KiB). The layout is a
static-asserted byte contract (cc/header.h), cross-checked from JS by
test/helpers.ts. `raw: true` opts out for foreign-process interop.

## Alternatives considered

- **fstat size checks (rev 1):** POSIX-only; no init protocol; no attach info.
- **Sidecar metadata files:** a second file per segment doubles the cleanup
  surface and breaks when only the segment is inherited (e.g. container
  --ipc sharing).
- **No header, magic in data region (shmbuf style):** user data starts at
  offset 0, so any metadata costs user bytes or a fixed offset — the same
  thing as a header, but undocumented and unversioned.

## Consequences

Segments are not byte-compatible with shmbuf or foreign tools (`raw: true`
covers that). Every segment costs one page. The data region stays
page-aligned, which keeps every TypedArray alignment legal and respects
16 KiB pages on Apple Silicon. The init protocol makes create/join races
benign and crash recovery deterministic (joiners take over a dead
initializer idempotently).
