# Ownerless Rename Index DDL Refresh

## Problem

Ownerless DDL coverage already proves standalone InnoDB secondary-index create
and drop are visible to an already-open peer. A narrower `ALTER TABLE` metadata
case remains useful: `ALTER TABLE ... RENAME INDEX` can use MariaDB's simple
rename/index-change path and updates dictionary/index metadata without adding or
dropping table rows. Ownerless mode needs evidence that this metadata-only index
rename crosses the dictionary-generation boundary and survives reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... RENAME INDEX old_name TO new_name` and marks
  `ALTER_RENAME_INDEX`.
- `mariadb/sql/sql_table.cc` routes standalone rename/index changes through
  `simple_rename_or_index_change()` when the operation does not need a full
  table-copy alter.
- MyLite ownerless DDL already marks the shared dictionary generation active
  before MariaDB executes DDL and publishes the next stable generation after
  success. Peers wait for the stable generation, refresh external page
  visibility, flush table metadata, and evict unused InnoDB dictionary entries
  before using the changed table.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER TABLE ... RENAME INDEX`.
- Verify an already-open peer sees the old index before the rename and the new
  index after the rename through `information_schema.statistics`.
- Verify `FORCE INDEX` fails for the old index name and succeeds for the new
  index name after peer refresh.
- Verify the renamed index and table rows survive ownerless reopen, ordinary
  exclusive reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not change ownerless DDL implementation or claim every MariaDB online DDL
  variant is covered.

## Design

Add a `rename-index-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table and secondary index, then
   signals the parent.
2. The already-open parent verifies the old index exists and can be forced.
3. The child runs
   `ALTER TABLE app.ownerless_rename_index_base RENAME INDEX
   ownerless_rename_old_idx TO ownerless_rename_new_idx`.
4. The parent verifies the old index is gone, the new index exists, the old
   `FORCE INDEX` form fails, the new `FORCE INDEX` form works, and later DML can
   use the renamed-index table.
5. After both ownerless peers close, helper assertions reopen through ownerless
   read/write, ordinary exclusive read/write, forced shared-memory rebuild, and
   ordinary exclusive read/write after rebuild.

## Compatibility Impact

No new public API behavior is added. The slice strengthens the partial
ownerless DDL compatibility claim for a metadata-only secondary-index rename
while keeping broader DDL/file-lifecycle support marked partial.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises existing
InnoDB `.frm`/`.ibd` metadata and ownerless concurrency files under the
MyLite-owned directory.

## Native Storage Impact

Native InnoDB storage format is unchanged. The renamed index remains a native
InnoDB secondary index managed by MariaDB; MyLite only coordinates the
ownerless dictionary boundary and reopen evidence.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `rename-index-ddl` in `embedded-dev`.
- Build and run focused `rename-index-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer observes old-index metadata before the child rename.
- The already-open peer observes new-index metadata after the child rename.
- `FORCE INDEX` rejects the old name and accepts the new name after refresh.
- The renamed-index table survives ownerless/native reopen before and after
  forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This slice does not cover every online DDL algorithm choice, index-order
  alteration, special indexes, partitioned tables, or durable file-lifecycle
  replay. Those remain separate partial-compatibility gaps.
