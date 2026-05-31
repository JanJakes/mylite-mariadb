# Ownerless Multi-Rename Cycle

## Problem

Ownerless DDL coverage now proves same-schema single-table rename and
cross-schema table movement. MariaDB also supports multi-table `RENAME TABLE`
statements where each pair is processed as one statement, allowing common swap
shapes such as `t1 TO tmp, t2 TO t1, tmp TO t2`. MyLite ownerless peers must
refresh dictionary and native file state after that statement without observing
stale table identity.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc` describes atomic rename of a table list and
  states that every two entries in the `TABLE_LIST` form an old/new rename
  pair.
- `mariadb/sql/sql_rename.cc` implements `rename_tables()` by iterating over
  those old/new pairs, checking each rename, and calling `do_rename()` for
  ordinary tables. On error, the caller reverts normal-table renames through the
  DDL log.
- `mariadb/sql/sql_parse.cc` implements `check_rename_table()` by walking the
  same old/new pairs and checking grants and temporary-table references before
  `mysql_rename_tables()` executes the statement.
- `mariadb/sql/sql_table.cc` implements `mysql_rename_table()`, which calls the
  storage-engine `ha_rename_table()` path and then renames the SQL `.frm`
  metadata file for each pair.
- `mariadb/storage/innobase/row/row0mysql.cc` and
  `mariadb/storage/innobase/dict/dict0dict.cc` update InnoDB dictionary names
  and rename file-per-table `.ibd` tablespaces through
  `dict_table_rename_in_cache()`.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for a three-pair table-name swap:
  `left TO tmp, right TO left, tmp TO right`.
- Already-open ownerless peer refresh of the final table names after the
  multi-rename statement.
- InnoDB `SPACE` identity checks proving the tablespaces swap names rather than
  only producing fresh tables with the same names.
- Peer writes through both swapped tables after the rename cycle.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Error-in-the-middle rollback injection for multi-rename.
- Cross-schema multi-rename cycles.
- View, trigger, partition, and temporary-table rename cycles.
- Crash injection during the rename statement.

## Design

- Add a focused `multi-rename-cycle` selector to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates two InnoDB tables with distinct rows and
  names under `app`.
- The parent keeps an already-open ownerless peer, records the two InnoDB
  `SPACE` values through `INFORMATION_SCHEMA.INNODB_SYS_TABLES`, and writes
  through the left table before the rename.
- The child executes one `RENAME TABLE` statement that rotates the left table
  through a temporary name and swaps the final `left` and `right` names.
- The parent verifies the final row contents and `SPACE` values swapped, the
  temporary name is absent from SQL metadata and native files, both final
  `.frm`/`.ibd` paths exist, and both swapped tables accept new peer writes.
- The final swapped state is verified through ownerless read/write and ordinary
  native exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for MariaDB multi-pair `RENAME TABLE`
statements. It does not claim complete atomic rename recovery for crash or
error-injection cases; those remain part of broader DDL recovery work.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies MariaDB native `.frm` and
`.ibd` files remain inside `datadir/app/`, the temporary rename target is gone
after the statement, and volatile shared-memory rebuild does not lose the
swapped state.

## Native Storage Impact

No storage-format changes. The slice exercises native InnoDB dictionary and
file-per-table rename behavior for multiple rename pairs in one SQL statement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `multi-rename-cycle` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes both source tables created by another
  process.
- The peer records distinct InnoDB `SPACE` values for the two source tables.
- After one multi-pair `RENAME TABLE` statement, final table names are present,
  the temporary name is absent, and final `SPACE` values are swapped.
- The peer sees the row contents under the swapped names and can write through
  both swapped tables.
- Final metadata, rows, and native files survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one deterministic swap shape. Error-path rollback, crash
  injection during the statement, and cross-schema multi-rename cycles remain
  follow-up DDL recovery work.
