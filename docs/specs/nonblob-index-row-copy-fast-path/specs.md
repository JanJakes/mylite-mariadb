# Non-BLOB Index Row Copy Fast Path

## Roadmap Slice

- Row and index storage
- SQL execution API performance
- Spec slug: `nonblob-index-row-copy-fast-path`

## Source Authority

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Relevant MyLite handler path:
  - `mariadb/storage/mylite/ha_mylite.cc`

## Problem

Prepared primary-key updates materialize the matched row through
`ha_mylite::read_index_cursor_row()` before calling `update_row()`. For
non-BLOB tables, the storage payload is already the fixed MariaDB record image,
but the handler still calls `mylite_scan_stored_row()` for a size check and then
calls `mylite_copy_stored_row_to_scan()`, which repeats the size check before
copying.

The focused prepared-update table is non-BLOB, and sampling still shows
indexed-row payload materialization and row-copy helpers on the hot path.

## Design

- In `read_index_cursor_row()`, detect non-BLOB tables before the generic
  BLOB-aware scan/copy path.
- Cache the opened handler table's BLOB/TEXT presence once and reuse it for
  index cursor materialization and retained-payload cleanup.
- For non-BLOB rows, validate `row_payload_size == table->s->reclength`, copy
  the payload directly into the target record buffer, and clear any retained
  BLOB payload slot for that record.
- Keep the existing BLOB/TEXT path unchanged, including descriptor validation,
  payload preservation, and owned payload cleanup.

## Compatibility Impact

No SQL-visible behavior changes. Non-BLOB rows already use the raw MariaDB
record image as the durable payload; the fast path performs the same fixed-size
validation and copy.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public API or file-format change.

## Test Plan

- Rebuild the storage-smoke MariaDB archive with `-DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run `mylite_storage_test`.
- Run the focused storage and embedded storage-engine CTest subset.
- Run `git diff --check`.
- Run `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`.
- Run a local prepared-update performance baseline.

## Acceptance Criteria

- Non-BLOB indexed row reads copy directly after fixed-size validation.
- Hot index cursor reads do not rescan table field metadata to decide whether
  the table has BLOB/TEXT columns.
- BLOB/TEXT indexed row reads keep the existing descriptor validation and
  payload-retention behavior.
- Existing storage, routed storage-engine, transaction, and rollback tests pass.

## Risks

- The fast path must still clear stale retained BLOB payload ownership for the
  target record buffer. It does this through the same record-slot bookkeeping
  used by the generic path.
