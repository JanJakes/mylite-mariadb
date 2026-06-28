# Ownerless FK Rename IF EXISTS Missing-Source Coverage

## Problem Statement

Ownerless foreign-key multi-rename coverage proves same-schema and cross-schema
parent/child rename recovery, including `RENAME TABLE IF EXISTS` lists where
all source tables exist. Ownerless missing-source `IF EXISTS` coverage proves
non-FK rename warning/no-op semantics. The remaining high-value gap is their
intersection: a foreign-key rename list where missing sources are skipped, real
parent/child pairs still execute, and a crash may land either before ownerless
dictionary finish or inside MariaDB's native rename loop.

## Source Findings

- Base source authority: MariaDB 11.8 import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. Missing
  source tables call `my_error(ER_NO_SUCH_TABLE, MYF(ME_NOTE), ...)` and return
  `-1` when `if_exists` is true. Existing targets remain hard errors.
- `mariadb/sql/sql_rename.cc:521-558` loops rename pairs. For non-temporary
  pairs it calls `check_rename(..., skip_error || if_exists)`, continues when
  the return value is negative, and still calls `do_rename()` for existing
  source/target pairs.
- `mariadb/sql/sql_rename.cc:395-405` records the DDL-log rename entry, calls
  `mysql_rename_table()`, then fires MyLite's native file-operation hook and
  the `rename-table-after-native-file-op` test fault after a successful native
  rename.

## Design

Add a bounded same-schema FK `RENAME TABLE IF EXISTS` coverage shape with three
missing source pairs interleaved around a real parent-through-temporary rename
chain:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_fk_multi_rename_missing_before
    TO app.ownerless_fk_multi_rename_missing_before_dst,
  app.ownerless_fk_multi_rename_parent
    TO app.ownerless_fk_multi_rename_parent_tmp,
  app.ownerless_fk_multi_rename_missing_middle
    TO app.ownerless_fk_multi_rename_missing_middle_dst,
  app.ownerless_fk_multi_rename_child
    TO app.ownerless_fk_multi_rename_child_moved,
  app.ownerless_fk_multi_rename_parent_tmp
    TO app.ownerless_fk_multi_rename_parent_moved,
  app.ownerless_fk_multi_rename_missing_after
    TO app.ownerless_fk_multi_rename_missing_after_dst;
```

The successful path must keep MariaDB note `1146` warnings for the skipped
sources, publish the moved parent/child metadata, preserve FK enforcement under
the generated moved constraint name, and leave skipped targets absent from SQL
metadata, InnoDB dictionary metadata, and native table files.

Hook-build recovery uses two focused crash windows:

- `dictionary-before-finish`: kill after MariaDB completes the mixed list and
  before ownerless dictionary finish, then recover the moved FK state while a
  peer is live.
- `rename-table-after-native-file-op` with
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`: kill after the second real native
  rename pair. Missing sources must not count toward hook skips. MariaDB
  DDL-log recovery should roll the partially executed parent/child chain back
  to the original FK state and keep all skipped targets absent.

## Affected Subsystems

- Ownerless cross-process SQL compatibility tests.
- Hook-build ownerless dictionary DDL crash selectors.
- Compatibility and ownerless concurrency documentation.

No product code change is expected unless the new selectors expose a recovery
bug. No public API, directory-layout, wire-protocol, dependency, or binary-size
impact is expected.

## Compatibility Impact

The SQL behavior remains MariaDB-compatible: `IF EXISTS` downgrades missing
source pairs to note `1146`, while existing parent/child rename pairs still run
through MariaDB's native rename and FK metadata refresh paths. The compatibility
matrix should classify this as covered same-schema FK missing-source
`RENAME TABLE IF EXISTS` evidence, while keeping broader FK, temporary, and
cross-schema longer permutations partial until tested.

## Database-Directory And Native Storage Impact

The production and crash selectors verify durable `.frm`/`.ibd` placement for
the original or moved FK tables, absence of skipped target files, and
ownerless/native reopen before and after forced `.shm` rebuild. The live-peer
crash selectors also verify the native file-operation checkpoint-needed marker
is retained while a peer remains open and drains only after no-live recovery.

## Test And Verification Plan

- Add production selector `foreign-key-rename-if-exists-missing-source`.
- Add hook selector
  `dictionary-fk-rename-if-exists-missing-source-crash`.
- Add hook selector
  `dictionary-fk-rename-if-exists-missing-source-loop-crash`.
- Register focused hook CTests for the two crash selectors. The loop selector
  CTest sets `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`.
- Run the production selector directly under a production embedded build.
- Run the two focused hook CTests under `ownerless-test-hooks`.
- Run the adjacent FK rename hook subset, production build guard,
  `format-check`, and `git diff --check`.

## Acceptance Criteria

- The production selector observes exactly three missing-source warnings,
  successful moved parent/child FK metadata, FK enforcement, skipped target
  absence, ownerless/native reopen, and forced `.shm` rebuild.
- The prefinish crash selector recovers the moved FK state while a peer is
  live, retains the native file-operation marker until peer release, drains the
  marker after no-live recovery, and preserves skipped target absence.
- The native-loop crash selector kills after the second real native rename,
  rolls back to the original FK state, preserves skipped target absence, keeps
  the marker retained/drained at the expected lifecycle points, and survives
  ownerless/native reopen plus forced `.shm` rebuild.

## Risks And Follow-Up

- This slice is same-schema only. Cross-schema longer missing-source FK lists
  remain planned.
- This slice does not cover mixed temporary/permanent FK rename lists.
- This slice does not broaden arbitrary FK plus non-FK ALTER list recovery,
  native redo/checkpoint reconciliation, active-reader pressure crash breadth,
  or external randomized MariaDB/RQG stress.
