# Ownerless CTAS Post-create DML

## Summary

Ownerless CTAS under a live stale snapshot pin must leave the created table
usable for later DML in the creating process and durable through ownerless and
native reopen. The slice fixes non-forced ownerless page-version write refresh
so an older page-version image cannot rewind a newer clean native page.

## Problem

`CREATE TABLE ... AS SELECT` can create and populate an InnoDB file-per-table
tablespace while another ownerless process pins an older repeatable-read
snapshot. The creating process can see the CTAS rows immediately, but a later
`UPDATE` of that CTAS destination could fail with MariaDB errno 1032,
`Can't find record`, when non-forced page-version write refresh copied an
older clean page-version image over a newer local/native page image.

The failure is narrower than a generic hidden-row-id or no-primary-key issue:
a same-shape no-primary-key table created with ordinary `CREATE TABLE` plus
`INSERT ... SELECT` continues to update under the same stale-reader pin.

## Source References

- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements ownerless InnoDB page refresh before writes.
- `mariadb/storage/innobase/row/row0upd.cc` reports
  `DB_RECORD_NOT_FOUND` when an update cursor cannot restore the expected
  clustered-record position.
- `mariadb/storage/innobase/row/row0mysql.cc` maps that update failure to the
  user-visible SQL error.
- `docs/specs/ownerless-created-tablespace-replay/specs.md` covers created
  tablespace final-state replay, but leaves exhaustive post-create DML matrices
  outside that slice.

## Design

Non-forced page-version write refresh remains allowed when the shared
page-version image is newer than the local buffer page, and when a clean page
has the same LSN as the page-version image so independent process-local redo
histories can still converge by full page image.

It no longer treats any different-LSN clean page-version image as acceptable.
If the local clean page carries a newer LSN than the page-version record, the
local/native page is the newer image and must not be overwritten by stale
ownerless WAL. Explicit forced refresh paths remain separate.

## Compatibility Impact

SQL behavior is unchanged. The slice tightens ownerless native-page refresh so
covered CTAS destinations remain updateable under retained stale-reader WAL.
Ownerless recovery remains partial for missing-file reconstruction and broader
DDL/file lifecycle classes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `ctas-post-create-dml` in `embedded-dev`.
- Run adjacent stale-reader replay selectors in `embedded-dev`.
- Build and run the same focused and adjacent selectors in
  `ownerless-test-hooks`.
- Run relevant ownerless stress and hygiene checks: `ownerless-stress`,
  `format-check`, `git diff --check`, cached diff checks, and temp/process
  cleanup checks.

## Acceptance Criteria

- Unpinned CTAS post-create `UPDATE` succeeds as a baseline.
- Under a live stale repeatable-read ownerless snapshot pin, ordinary
  no-primary-key `CREATE TABLE` plus `INSERT ... SELECT` remains updateable.
- Under that same pin, CTAS post-create numeric and payload updates succeed.
- Writer close retains page-version WAL while the stale reader pin is live.
- After the reader releases, ownerless/native reopen before and after forced
  `.shm` rebuild all observe the CTAS destination, 3 rows, `SUM(id)=6`,
  `SUM(value)=1227`, and 12,000 payload bytes.

## Out of Scope

- Exhaustive CTAS DML matrix coverage.
- Crash injection inside CTAS or the post-create DML.
- Reconstructing a missing CTAS `.ibd` only from page-version WAL.
- External MariaDB/RQG oracle execution.
