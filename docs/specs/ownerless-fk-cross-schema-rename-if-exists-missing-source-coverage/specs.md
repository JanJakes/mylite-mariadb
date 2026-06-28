# Ownerless FK Cross-Schema Rename IF EXISTS Missing-Source Coverage

## Problem Statement

Ownerless DDL recovery now covers cross-schema foreign-key multi-renames when
all sources exist, and same-schema foreign-key `RENAME TABLE IF EXISTS` lists
with interleaved missing sources. The remaining adjacent gap is a cross-schema
foreign-key rename list where skipped missing-source pairs are interleaved with
real parent/child moves into another schema. This matters because the recovery
path must reconcile MariaDB's DDL log, InnoDB FK metadata, target-schema native
files, and skipped target absence across multiple schema directories.

## Source Findings

- Base source authority: MariaDB 11.8 import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. A missing
  source table emits `ER_NO_SUCH_TABLE` as a note and returns `-1` when
  `if_exists` is true. Existing targets remain non-skippable errors.
- `mariadb/sql/sql_rename.cc:521-558` iterates rename pairs. For
  non-temporary pairs it skips negative `check_rename()` results, but still
  calls `do_rename()` for real source/target pairs later in the list.
- `mariadb/sql/sql_rename.cc:395-405` logs the DDL rename, performs
  `mysql_rename_table()`, and then fires MyLite's native file-operation hook
  plus the `rename-table-after-native-file-op` test fault after a successful
  native rename.

## Design

Add a focused cross-schema FK `RENAME TABLE IF EXISTS` coverage shape with
three missing source pairs around two real parent/child moves:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_fk_cross_schema_multi_missing_before
    TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_missing_before_dst,
  app.ownerless_fk_cross_schema_multi_parent
    TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_parent_moved,
  ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_missing_middle
    TO app.ownerless_fk_cross_schema_multi_missing_middle_dst,
  app.ownerless_fk_cross_schema_multi_child
    TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_child_moved,
  app.ownerless_fk_cross_schema_multi_missing_after
    TO app.ownerless_fk_cross_schema_multi_missing_after_dst;
```

The successful path must prove three missing-source notes, moved parent/child
metadata in the target schema, generated moved FK constraint enforcement, and
absence of every skipped target from SQL metadata, InnoDB dictionary metadata,
and native `.frm`/`.ibd` files in both `app` and the target schema.

Hook-build recovery should add two crash selectors:

- `dictionary-before-finish`: kill after MariaDB completes the mixed list and
  before ownerless dictionary finish, then recover the moved cross-schema FK
  state while another ownerless peer remains live.
- `rename-table-after-native-file-op` with
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`: kill after the second real native
  rename. Missing sources must not count toward the skip budget. MariaDB
  DDL-log recovery should roll the parent/child move back to the original
  `app` schema state and keep all skipped targets absent.

## Affected Subsystems

- Ownerless cross-process SQL compatibility tests.
- Hook-build ownerless dictionary DDL crash selectors.
- Compatibility and ownerless concurrency documentation.

No product code, public API, directory-layout, dependency, wire-protocol, or
binary-size change is expected unless the selectors expose a recovery defect.

## Compatibility Impact

The SQL semantics remain MariaDB-compatible: `IF EXISTS` downgrades missing
source pairs to note `1146`, and real cross-schema parent/child rename pairs
still execute through MariaDB's native rename and FK metadata refresh paths.
The compatibility matrix should classify this as covered cross-schema FK
missing-source `RENAME TABLE IF EXISTS` evidence. Longer mixed
temporary/permanent rename lists and randomized external stress remain planned.

## Database-Directory And Native Storage Impact

The production and crash selectors verify durable placement or absence of
`.frm` and `.ibd` files under the source and target schema directories. They
also verify ownerless/native reopen and forced `.shm` rebuild behavior. The
crash selectors verify native file-operation checkpoint marker retention while
a live peer is held and marker drain after final no-live recovery.

## Test And Verification Plan

- Add production selector
  `foreign-key-cross-schema-rename-if-exists-missing-source`.
- Add hook selector
  `dictionary-fk-cross-schema-rename-if-exists-missing-source-crash`.
- Add hook selector
  `dictionary-fk-cross-schema-rename-if-exists-missing-source-loop-crash`.
- Register focused hook CTests for the two crash selectors. The loop selector
  CTest sets `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`.
- Run the production selector directly under `embedded-prod` and the matching
  `sql-case` selector under `php-embedded-prod`.
- Run the two focused hook CTests and the adjacent FK cross-schema rename hook
  subset under `ownerless-test-hooks`.
- Run `tools.ci-production-builds`, `format-check`, and `git diff --check`.

## Acceptance Criteria

- The production selector observes exactly three missing-source warnings,
  successful cross-schema moved parent/child FK metadata, FK enforcement,
  skipped target absence in SQL metadata, InnoDB metadata, and native files,
  ownerless/native reopen, and forced `.shm` rebuild.
- The prefinish crash selector recovers the moved target-schema FK state while
  a peer is live, retains the native file-operation marker until peer release,
  drains the marker after no-live recovery, and preserves skipped target
  absence.
- The native-loop crash selector kills after the second real native rename,
  rolls back to the original `app` schema FK state, preserves skipped target
  absence, keeps marker retained/drained at the expected lifecycle points, and
  survives ownerless/native reopen plus forced `.shm` rebuild.

## Risks And Follow-Up

- This slice covers one deterministic cross-schema FK missing-source shape, not
  exhaustive permutations of missing source placement.
- Mixed temporary/permanent rename lists remain planned.
- Broader native redo/checkpoint reconciliation, arbitrary DDL file lifecycle,
  active-reader pressure breadth, and external MariaDB/RQG randomized stress
  remain planned.
