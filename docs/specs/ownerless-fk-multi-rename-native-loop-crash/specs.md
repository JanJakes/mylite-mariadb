# Ownerless FK Multi-Rename Native-Loop Crash

## Problem

Ownerless foreign-key multi-rename crash coverage already kills writers after
MariaDB completes the native `RENAME TABLE` list and before MyLite finishes the
ownerless dictionary generation. That proves the completed-DDL boundary, but it
does not prove what happens if the process dies inside MariaDB's same-statement
rename loop after only the first native file operation has completed.

This slice closes one bounded native-loop gap: a parent/child foreign-key
rename chain where the writer is killed after the first native rename succeeds
but before the rest of the list runs. On reopen, MariaDB's DDL log reverts that
incomplete rename list back to the original parent/child state; MyLite must
preserve that native outcome, not synthesize the completed rename list.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` rejects locked-table and active-transaction
    contexts, then drives the `TABLE_LIST` old/new pairs through
    `rename_tables()`.
  - `rename_tables()` processes every two list entries as one old/new pair in a
    loop and calls `do_rename()` for ordinary non-temporary tables.
  - `do_rename()` calls `mysql_rename_table()` for each pair, then updates the
    DDL-log phase, triggers, and statistics after the native table move
    succeeds.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` builds schema-qualified old/new native paths, calls
    the storage-engine `ha_rename_table()` path, and renames the `.frm`
    metadata file.
  - Existing MyLite fork-local drop-loop coverage calls
    `mylite_ownerless_dictionary_native_file_op()` and the unsafe
    `drop-table-after-native-file-op` test fault after a successful
    non-temporary table file operation.
- `mariadb/storage/innobase/handler/ha_innodb.cc` and
  `mariadb/storage/innobase/row/row0mysql.cc`
  - InnoDB's table rename path updates native dictionary metadata and
    foreign-key references for the renamed table using normalized `db/table`
    identifiers before reporting success to SQL.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` marks a dictionary generation active
    before MariaDB executes DDL.
  - `ownerless_dictionary_native_file_op_hook()` persists the native file-op
    checkpoint-needed marker and marks the active dictionary generation
    recoverable.
  - Dead-owner dictionary recovery completes that ownerless generation; it does
    not replay or synthesize the remaining native rename pairs.

## Scope And Non-Goals

In scope:

- Add a MyLite fork-local unsafe test fault immediately after each successful
  normal-table `mysql_rename_table()` call in MariaDB's `RENAME TABLE` loop.
- Hook-build SQL coverage for a same-schema three-pair parent/child foreign-key
  rename chain:

  ```sql
  RENAME TABLE
    app.ownerless_fk_multi_rename_parent
      TO app.ownerless_fk_multi_rename_parent_tmp,
    app.ownerless_fk_multi_rename_child
      TO app.ownerless_fk_multi_rename_child_moved,
    app.ownerless_fk_multi_rename_parent_tmp
      TO app.ownerless_fk_multi_rename_parent_moved
  ```

- Kill the writer after the first successful native rename and before the
  second pair runs.
- Verify live-ownerless recovery while another ownerless peer remains open.
- Verify MariaDB DDL-log recovery reverts the interrupted rename list to a
  usable original state: the original parent and child exist, the intermediate
  and final moved names are absent, the child FK still references the original
  parent, rows are preserved, FK enforcement still works, and native files
  match the reverted state.
- Verify the native file-op marker remains set while the live peer exists and
  drains only after final no-live ownerless recovery.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Proving every possible crash point in MariaDB's rename-list rollback path.
- Cross-schema FK native-loop rename crashes.
- Completed native rename-list crash recovery, which existing
  `dictionary-foreign-key-multi-rename-crash` coverage already proves.
- Full durable DDL file-lifecycle metadata and randomized external DDL/RQG
  stress.
- SQL-level table-lock wait fault injection.

## Design

- Include `mylite_ownerless_dictionary_hooks.h` in `mariadb/sql/sql_rename.cc`
  and declare the existing `mylite_ownerless_innodb_test_fault()` hook.
- After each successful `mysql_rename_table()` call in `do_rename()`, call:
  - `mylite_ownerless_dictionary_native_file_op()`
  - `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
- Add a hook-only `dictionary-fk-multi-rename-loop-crash` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The selector reuses the same parent/child names as the completed FK
  multi-rename crash test but kills the writer at
  `rename-table-after-native-file-op` with no skip count, so the first pair is
  the interruption point.
- Assertions target MariaDB's actual reverted native state:
  - `ownerless_fk_multi_rename_parent` exists and owns the original parent
    rows.
  - `ownerless_fk_multi_rename_child` exists and owns the original child rows.
  - `ownerless_fk_multi_rename_parent_tmp`, `..._parent_moved`, and
    `..._child_moved` are absent.
  - `REFERENTIAL_CONSTRAINTS` uses the original child constraint identity and
    references `ownerless_fk_multi_rename_parent`.
  - Valid child inserts succeed and missing-parent/parent-delete operations
    fail with the expected MariaDB foreign-key errors.

## Compatibility Impact

No SQL syntax or public C API behavior changes. This expands hook-build
ownerless concurrency evidence from completed rename-list crash boundaries to a
same-statement native-loop FK rename boundary where MariaDB DDL-log recovery
rolls back the first native rename. Overall DDL/file lifecycle recovery remains
partial until broader native-loop, ALTER rebuild, schema lifecycle, and
external oracle coverage are complete.

## Directory And Lifecycle Impact

No directory layout changes. The selector verifies all durable `.frm` and
`.ibd` files remain inside the MyLite database directory, and that the reverted
original state survives live-peer recovery, no-live marker drain, forced
shared-memory rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

No storage-format changes. MariaDB native InnoDB metadata and DDL-log recovery
remain the authority for the interrupted rename-list outcome.

## Embedded Lifecycle And API

No public API changes. The test exercises embedded ownerless open/close around
a killed DDL writer, live peer retention, no-live recovery, forced `.shm`
rebuild, and ordinary native reopen.

## Build, Size, And Dependencies

No production dependency or license impact. The MariaDB fork delta is a narrow
test-hook call in an existing DDL loop and is compiled into the existing unsafe
hook build. Normal behavior is unchanged unless ownerless hooks and the unsafe
test fault are enabled.

## Test Plan

- Build the hook preset containing `mylite_ownerless_cross_process_sql_test`.
- Run the focused `dictionary-fk-multi-rename-loop-crash` selector.
- Run adjacent completed FK multi-rename crash selectors.
- Run the existing multi-drop-loop selectors to guard the similar native-loop
  drop boundary.
- Run the hook CTest subset for the new selector and adjacent rename/drop loop
  selectors.
- Run focused product FK multi-rename selectors.
- Run format and diff checks.

## Acceptance Criteria

- The new fault hook kills a writer after the first native rename pair in the
  FK multi-rename chain.
- A live ownerless opener can recover the dead writer's dictionary generation
  while another peer remains open.
- MariaDB's reverted original state is preserved and usable; recovery does not
  invent the remaining rename pairs.
- FK metadata and enforcement reference the original parent table.
- The native file-op checkpoint marker remains durable while a live peer is
  present and drains after final no-live recovery.
- Ownerless and native reopen, including after forced `.shm` rebuild, observe
  the same reverted state.

## Risks And Open Questions

- This slice originally proved one deterministic same-schema FK native-loop
  rollback point. Later same-schema skip-count coverage, cross-schema FK loop
  coverage, and existing-table FK `IF EXISTS` lists are covered by follow-up
  specs; rollback after ordinary SQL errors remains outside this slice.
- The new MariaDB hook is fork-local and should remain limited to unsafe
  ownerless hook builds.
- Broader DDL/file-lifecycle recovery, active-reader pressure matrices, and
  external randomized MariaDB/RQG stress remain completion work.
