# Active Indexed Packed Row Append

## Problem

Active no-index inserts can now pack fixed-size rows into buffered version-`2`
row pages, but indexed inserts still use the legacy one-row writer. That means
ordinary application tables with primary or secondary keys do not yet benefit
from packed row ids or reduced row-page write amplification.

The indexed packed writer must choose the marked row id before index planning,
then write append-only index entries and maintained roots with that exact row
id.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:3929-4086` prepares index entries
  before calling `mylite_storage_append_row_with_index_entries()`, then stores
  the returned row id in `current_row_id`.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  predicts the inserted row id before `plan_maintained_index_root_inserts()`.
- Earlier packed index work changed append-only index-entry, maintained-root,
  and published-leaf decoders to validate opaque row references rather than
  raw physical page ids.
- `write_inline_insert_pages()` owns the active append-buffer fast path for one
  row page plus append-only index-entry pages.

## Design

- Replace the insert-only physical page-id prediction with a row-reference
  prediction:
  - predict a marked cached-page slot when the active packed append cache can
    accept the row and any append-only index pages can be reserved without
    flushing the buffer;
  - predict a marked slot-`0` id when a new packed row page plus append-only
    index pages can be reserved in the active append buffer;
  - otherwise predict the existing legacy physical page id.
- Use that predicted row reference for maintained index-root planning.
- Extend the packed inline insert writer to:
  - append to a cached packed row page and write append-only index-entry pages
    with the marked slot row id;
  - create a new packed row page and write append-only index-entry pages in the
    same reserved append run;
  - fall back to the existing legacy inline writer when packed preconditions do
    not hold.
- Keep oversized rows and non-buffered direct appends on the legacy writer.

## Affected Subsystems

- Active fixed-size insert writer.
- Append-only index-entry publication.
- Maintained root/branch insert planning row-id input.
- Exact indexed reads through existing packed row-reference materialization.

## Compatibility Impact

No SQL-visible behavior change is intended. Row references remain opaque
eight-byte handler refs. Exact index lookup and duplicate-key filtering should
see the same logical rows while storing marked row references for packed rows.

## Single-File And Lifecycle Impact

No new sidecars or lifecycle states. Packed row pages and index-entry pages are
ordinary durable pages in the primary `.mylite` file, with rollback mediated by
the existing append buffer and journal machinery.

## Public API And File-Format Impact

No public API signature change. Production active fixed-size indexed inserts
can now emit row-page version `2` and marked row ids inside existing 64-bit
index-entry fields.

## Storage-Engine Routing Impact

No routing policy change. Routed durable tables may receive packed indexed rows
when they use the active append-buffer path.

## Binary-Size Impact

Small first-party storage changes only. No dependency change.

## Tests And Verification Plan

- Add storage coverage for active fixed-size inserts with append-only index
  entries:
  - returned row ids are marked packed references;
  - exact indexed lookup materializes each packed row;
  - committed file grows by one packed row page plus one index-entry page per
    inserted row.
- Add delete visibility for one indexed packed row.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Active fixed-size indexed inserts can share one version-`2` packed row page.
- Append-only index-entry pages store marked packed row references.
- Exact indexed lookup returns packed rows before and after commit.
- Delete visibility filters stale indexed packed entries.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.

## Risks And Unresolved Questions

- If a packed append would require flushing the active append buffer, this
  slice falls back to the legacy row writer rather than mutating flushed packed
  pages.
- BLOB/TEXT, variable-sized packed layouts, and durable free-space reuse remain
  separate work.
- Broader maintained branch split coverage should continue in dedicated index
  slices even though the row-id plumbing here is opaque-reference-safe.
