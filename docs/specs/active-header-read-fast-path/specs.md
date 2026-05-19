# Active Header Read Fast Path

## Problem

Update-heavy routed storage workloads still spend visible CPU in page-header
codec and checksum work while an active MyLite checkpoint already owns the
current decoded header. Sampling after the FK-presence and leaf-entryset
optimizations showed repeated `read_header()` calls under active statements
flowing through `read_page_at()`, `encode_header_page()`, and
`decode_header_page()` before returning the same in-memory
`statement->current_header`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` reaches storage update
  paths inside a MyLite statement checkpoint opened by
  `mylite_begin_statement_checkpoint()`.
- `packages/mylite-storage/src/storage.c::begin_checkpoint()` records the
  checkpoint-start header and initializes `statement->current_header`.
- `write_page_at()` intercepts active page-0 writes, decodes the supplied
  header once, and stores it as `statement->current_header` without publishing
  page 0 until checkpoint commit or rollback.
- `read_page_at()` already serves active header reads from checkpoint memory,
  but it encodes that decoded header into a temporary page. `read_header()`
  then immediately decodes and checksums the temporary page again.
- Active read snapshots and transaction-journal snapshots also keep decoded
  headers in memory alongside their saved header pages.

## Design

- Add a top-level `read_header()` fast path for active read snapshots,
  transaction-journal snapshots, and active statements with a current header.
- Return the already-decoded in-memory header directly for those cases.
- Preserve the existing page read and decode path for normal durable reads,
  corrupted files, and inactive file handles.
- Do not add a blanket active-statement catalog-root shortcut in this slice:
  catalog root pages can be rewritten during DDL, and the current statement
  structure only stores the checkpoint-start catalog page.

## Compatibility Impact

SQL-visible behavior is unchanged. The fast path returns the same header state
that the previous active path encoded and decoded. Statement commit, rollback,
savepoint propagation, recovery journals, and transaction-journal read
snapshots keep their existing publication semantics.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The change only avoids
redundant transient header serialization for already-active storage lifetimes.

## Test And Verification Plan

- Add a focused storage unit check that `mylite_storage_open_header()` observes
  an active statement's current page count before rollback without publishing
  that page count durably after rollback.
- Rebuild and run the storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline to measure update-path impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Active checkpoint and snapshot header reads return decoded in-memory headers
  without page encode/decode checksum work.
- Active statement header visibility remains correct before commit and after
  rollback.
- Storage and storage-engine compatibility tests pass.
- Update-heavy benchmark timings improve or stay within noise without changing
  row, catalog, or recovery behavior.

## Risks

- This removes a hot redundant header path but does not address repeated
  catalog validation, row-page checksums, or durable page I/O. Those require
  follow-up pager, catalog-current-state, and navigable-index slices.
