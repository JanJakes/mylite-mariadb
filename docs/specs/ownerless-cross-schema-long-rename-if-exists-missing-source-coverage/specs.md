# Ownerless Cross-Schema Long Rename IF EXISTS Missing-Source Coverage

## Problem Statement

Ownerless rename recovery covers same-schema longer `RENAME TABLE IF EXISTS`
missing-source lists and focused cross-schema missing/existing/missing-source
lists. The remaining adjacent cross-schema gap is a longer list with skipped
missing sources around more than one real native file move across schema
directories. This matters because MariaDB's rename loop must skip absent
sources without treating them as native file operations, while MyLite recovery
must reconcile both moved file-per-table tablespaces and skipped target
absence.

## Source Findings

- Base source authority: MariaDB 11.8 import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. For
  `RENAME TABLE IF EXISTS`, missing source tables emit `ER_NO_SUCH_TABLE` as
  notes and return `-1`, while real source/target pairs continue.
- `mariadb/sql/sql_rename.cc:521-558` skips negative `check_rename()` results
  and calls `do_rename()` only for real pairs.
- `mariadb/sql/sql_rename.cc:395-405` performs the native rename and then
  fires MyLite's native file-operation hook and
  `rename-table-after-native-file-op` test fault. Skipped missing-source pairs
  must not consume the native-hook skip budget.

## Design

Add a deterministic cross-schema longer missing-source shape:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_xrn_if_exists_long_missing_a
    TO app_archive.ownerless_xrn_if_exists_long_missing_a_dst,
  app.ownerless_xrn_if_exists_long_left_src
    TO app_archive.ownerless_xrn_if_exists_long_left_dst,
  app_archive.ownerless_xrn_if_exists_long_missing_b
    TO app.ownerless_xrn_if_exists_long_missing_b_dst,
  app_archive.ownerless_xrn_if_exists_long_right_src
    TO app.ownerless_xrn_if_exists_long_right_dst,
  app.ownerless_xrn_if_exists_long_missing_c
    TO app_archive.ownerless_xrn_if_exists_long_missing_c_dst;
```

The successful path proves three missing-source warnings, the left table moved
from `app` to `app_archive`, the right table moved from `app_archive` to
`app`, skipped target absence in SQL/InnoDB metadata and native files, and
ownerless/native reopen before and after forced `.shm` rebuild.

Hook-build recovery adds:

- a `dictionary-before-finish` crash selector proving live-peer recovery of
  both completed cross-schema moves while preserving skipped target absence;
- a `rename-table-after-native-file-op` crash selector with a one-real-op skip,
  proving the second real native file operation was reached and MariaDB DDL-log
  rollback restores both original schema placements and InnoDB `SPACE`
  identities while skipped targets remain absent.

## Affected Subsystems

- Ownerless cross-process SQL compatibility tests.
- Hook-build ownerless dictionary DDL crash selectors.
- Compatibility and ownerless concurrency documentation.

No product code, public API, directory-layout, dependency, wire-protocol, or
binary-size change is expected unless the selectors expose a recovery defect.

## Compatibility Impact

This preserves MariaDB behavior: `IF EXISTS` downgrades missing source pairs to
warnings and continues executing later real pairs. The compatibility matrix
should classify cross-schema longer missing-source `RENAME TABLE IF EXISTS`
lists as covered. Mixed temporary/permanent rename-list variants and external
randomized DDL stress remain planned.

## Database-Directory And Native Storage Impact

The selectors verify durable `.frm`/`.ibd` placement or absence under both
`datadir/app` and `datadir/app_archive`. The native-loop selector verifies
rollback to the original source schema directories and original InnoDB `SPACE`
identities after the second real native rename.

## Test And Verification Plan

- Add production selector `cross-schema-rename-if-exists-long-missing-noop`.
- Add hook selector `dictionary-cross-schema-rename-if-exists-long-missing-crash`.
- Add hook selector
  `dictionary-cross-schema-rename-if-exists-long-missing-loop-crash`.
- Register focused hook CTests for the two crash selectors.
- Run the production selector directly under `embedded-prod` and the matching
  `sql-case` selector under `php-embedded-prod`.
- Run the focused hook CTests and adjacent IF EXISTS rename crash subset under
  `ownerless-test-hooks`.
- Run `tools.ci-production-builds`, `format-check`, and `git diff --check`.

## Acceptance Criteria

- The production selector observes exactly three warnings, two completed
  cross-schema moves, skipped target absence from SQL metadata, InnoDB
  metadata, and native files, plus ownerless/native reopen and forced `.shm`
  rebuild.
- The prefinish crash selector recovers both moved tables while a peer is live,
  retains the native file-operation marker until peer release, drains it after
  no-live recovery, and preserves skipped target absence.
- The native-loop crash selector kills after the second real native rename,
  restores both original schema placements and `SPACE` identities, preserves
  skipped target absence, drains the marker after no-live recovery, and
  survives ownerless/native reopen plus forced `.shm` rebuild.

## Risks And Follow-Up

- This is a deterministic longer cross-schema shape, not exhaustive
  permutation testing.
- Mixed temporary/permanent `RENAME TABLE IF EXISTS` lists still need separate
  coverage because MariaDB temporary-table routing follows a different branch.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, and
  randomized external MariaDB/RQG stress remain planned.
