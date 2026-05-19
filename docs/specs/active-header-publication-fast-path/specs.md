# Active Header Publication Fast Path

## Problem

Active row mutations update `header.page_count` after appending row, row-state,
index-entry, or autoincrement pages. The hot update path still encodes a header
page, calls `write_page_at()`, and immediately decodes that header page again
because active statements keep page `0` unpublished in memory until checkpoint
commit. Sampling after the active catalog-root cache showed this
encode/decode checksum pair as a remaining per-row cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`,
  `mylite_storage_update_row_with_index_entries()`, and
  `mylite_storage_delete_row()` encode a header page and pass it to
  `write_page_at()` after changing `header.page_count`.
- `write_page_at()` decodes active header page writes to update
  `statement->current_header` without publishing page `0`.
- `publish_header_page_count()` uses the same encode-then-`write_page_at()`
  pattern for truncate and autoincrement state publication.
- `write_statement_current_header()` is the checkpoint commit path that must
  still encode and write the final header page to the primary file.

## Design

- Add an internal `publish_header()` helper that publishes a decoded header.
- When an active statement owns the file, update `statement->current_header`
  directly, mark it dirty, and preserve the existing catalog-cache invalidation
  rule when catalog root page or generation changes.
- When no active statement owns the file, encode the header once and write page
  `0` directly through the raw page writer.
- Route row append, row update, row delete, and `publish_header_page_count()`
  through the helper.
- Keep `write_page_at()` header interception for existing DDL and recovery
  paths that already construct a full header page.

## Compatibility Impact

SQL-visible behavior is unchanged. Active statements still expose the current
header to same-owner storage calls and publish page `0` only at commit,
rollback, or savepoint propagation boundaries. Non-active writes still write a
checksummed header page to the primary file.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The helper only removes
redundant transient header serialization for active storage checkpoints.

## Test And Verification Plan

- Existing statement checkpoint tests cover active header visibility before
  rollback, durable rollback of page count, row append/update/delete
  visibility, truncate publication, and autoincrement state publication.
- Rebuild and run the storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline to measure update and insert impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Active row mutation header publication no longer performs an encode/decode
  checksum pair just to update `statement->current_header`.
- Existing active statement header visibility and rollback semantics remain
  covered.
- Storage and storage-engine compatibility tests pass.
- Update-heavy benchmark timings improve or stay within noise without changing
  row, catalog, or recovery behavior.

## Risks

- This does not remove the real final header checksum at checkpoint commit or
  non-active publication. It only removes redundant active-statement header
  serialization.
