# Ownerless FK Cross-Schema Rename Native-Loop Crash

## Problem

Ownerless foreign-key multi-rename crash coverage now includes a same-schema
native-loop crash after the first successful MariaDB rename pair. Cross-schema
foreign-key multi-rename still only proves the completed native rename-list
boundary, where MariaDB has already moved both parent and child into the target
schema before MyLite finishes the ownerless dictionary generation.

This slice closes the first cross-schema native-loop gap: kill the writer after
the first native parent rename succeeds, before the child rename runs. On
reopen, MariaDB's DDL log must roll that incomplete list back to the original
schema-local parent/child state. MyLite must preserve that native outcome,
retain the ownerless native file-operation marker while a peer is live, and
drain it only after final no-live recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `rename_tables()` iterates old/new pairs and calls `do_rename()` for
    non-temporary tables.
  - `do_rename()` writes the DDL-log rename record, calls
    `mysql_rename_table()`, then advances the DDL-log phase for triggers and
    statistics after the native table rename succeeds.
  - MyLite now calls `mylite_ownerless_dictionary_native_file_op()` and the
    unsafe `rename-table-after-native-file-op` test fault immediately after a
    successful `mysql_rename_table()` call.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` executes the engine rename and `.frm` rename using
    schema-qualified old/new native paths.
- `mariadb/storage/innobase/handler/ha_innodb.cc` and
  `mariadb/storage/innobase/row/row0mysql.cc`
  - InnoDB updates native dictionary table names and foreign-key references
    during rename using normalized `db/table` identifiers.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL recovery completes the MyLite dictionary
    generation around MariaDB's recovered native state; it does not synthesize
    the rest of an interrupted MariaDB rename list.

## Scope And Non-Goals

In scope:

- Add hook-build SQL coverage for this two-pair cross-schema FK rename:

  ```sql
  RENAME TABLE
    app.ownerless_fk_cross_schema_multi_parent
      TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_parent_moved,
    app.ownerless_fk_cross_schema_multi_child
      TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_child_moved
  ```

- Kill the writer at `rename-table-after-native-file-op` after the first native
  parent rename succeeds.
- Verify live-ownerless recovery while another ownerless peer remains open.
- Verify MariaDB DDL-log recovery restores the original `app` parent and child,
  leaves the target schema present but without the moved tables, preserves rows,
  and preserves FK metadata/enforcement against the original parent.
- Verify the native file-op marker remains durable while the peer is live and
  drains after final no-live recovery.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Later-pair cross-schema rename-loop crash points.
- Same-schema native-loop rollback, which is covered by
  `ownerless-fk-multi-rename-native-loop-crash`.
- Completed cross-schema rename-list crash recovery, which existing
  `dictionary-foreign-key-cross-schema-multi-rename-crash` coverage proves.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, active
  reader policy breadth, or external randomized DDL/RQG stress.
- SQL-level table-lock wait fault injection.

## Design

- Reuse the existing fork-local `rename-table-after-native-file-op` hook in
  `do_rename()`.
- Add a hook-only `dictionary-fk-cross-schema-multi-rename-loop-crash` selector
  to `mylite_ownerless_cross_process_sql_test`.
- Reuse the existing cross-schema FK table names and target schema so the new
  test is adjacent to the completed-list crash coverage.
- Add a loop-specific assertion helper for the reverted state:
  - original parent and child tables exist in `app`;
  - target moved parent and child tables are absent;
  - original `.frm` and `.ibd` files exist under `app`;
  - target moved files are absent;
  - `REFERENTIAL_CONSTRAINTS` still references the original parent;
  - valid child inserts succeed and missing-parent/parent-delete writes fail
    with MariaDB FK errors;
  - ownerless and native reopen observe the same state after marker drain and
    forced `.shm` rebuild.

## Compatibility Impact

No SQL syntax or public C API behavior changes. The slice expands ownerless
crash-recovery evidence for MariaDB-compatible `RENAME TABLE` semantics at a
cross-schema FK native-loop rollback boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test verifies durable table files stay inside
the MyLite database directory, that the empty target schema directory remains a
valid MariaDB schema artifact, and that the reverted native state survives
live-peer recovery, no-live marker drain, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

No storage-format changes. MariaDB's DDL log and InnoDB dictionary recovery are
the authority for the interrupted cross-schema rename outcome.

## Embedded Lifecycle And API

No public API changes. The selector exercises embedded ownerless open/close,
dead-writer recovery with a live peer, no-live recovery, forced shared-memory
rebuild, and ordinary native reopen.

## Build, Size, And Dependencies

No production dependency or license impact. The slice adds hook-build test
coverage only; the MariaDB hook was already added by the preceding same-schema
native-loop slice.

## Test Plan

- Build the hook preset containing `mylite_ownerless_cross_process_sql_test`.
- Run the focused `dictionary-fk-cross-schema-multi-rename-loop-crash`
  selector.
- Run adjacent FK rename crash selectors.
- Run focused product FK multi-rename selectors to guard non-hook builds.
- Run format and diff checks.

## Acceptance Criteria

- The new selector kills a cross-schema FK rename writer after the first native
  parent rename pair.
- Live ownerless recovery preserves MariaDB's reverted original state, not the
  completed target-schema state.
- FK metadata and enforcement reference the original `app` parent table.
- The target schema remains present while moved parent/child files and metadata
  are absent.
- The native file-op checkpoint marker remains durable while a live peer is
  present and drains after final no-live recovery.
- Ownerless and native reopen, including after forced `.shm` rebuild, observe
  the same reverted state.

## Risks And Open Questions

- This proves the first cross-schema FK native-loop rollback point only.
  Later-pair skip-count coverage and broader DDL/file lifecycle recovery remain
  planned.
- The hook is fork-local and must stay constrained to unsafe ownerless hook
  tests.
