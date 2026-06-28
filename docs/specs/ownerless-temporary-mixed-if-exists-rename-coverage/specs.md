# Ownerless Temporary Mixed IF EXISTS Rename Coverage

## Problem Statement

Ownerless crash coverage already includes pure temporary rename chains and
mixed temporary/permanent `RENAME TABLE` lists. The remaining adjacent gap is a
mixed temporary/permanent `RENAME TABLE IF EXISTS` list where missing permanent
sources are skipped as warnings while temporary and permanent real pairs still
execute. This matters because MariaDB routes temporary source pairs through a
different branch from durable table pairs, while MyLite must keep the durable
native file-operation marker semantics for the permanent move and avoid
materializing skipped missing-source targets.

## Source Findings

- Base source authority: MariaDB 11.8 import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. With
  `IF EXISTS`, missing durable source tables emit `ER_NO_SUCH_TABLE` notes and
  return `-1` so the pair can be skipped.
- `mariadb/sql/sql_rename.cc:521-558` routes `is_temporary_table()` source
  pairs directly through `do_rename_temporary()` and stores them only in a
  temporary revert list. Durable pairs call `check_rename()` and then
  `do_rename()`.
- `mariadb/sql/sql_rename.cc:395-405` performs durable native renames and
  fires MyLite's native file-operation hook plus the
  `rename-table-after-native-file-op` test fault. Temporary-only pairs do not
  provide durable native file-op evidence.

## Design

Add a deterministic mixed statement:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_tmp_if_exists_missing_before
    TO app.ownerless_tmp_if_exists_missing_before_dst,
  app.ownerless_tmp_if_exists_shadow
    TO app.ownerless_tmp_if_exists_shadow_moved,
  app.ownerless_tmp_if_exists_perm_src
    TO app.ownerless_tmp_if_exists_perm_dst,
  app.ownerless_tmp_if_exists_missing_after
    TO app.ownerless_tmp_if_exists_missing_after_dst;
```

The session owns a temporary `ownerless_tmp_if_exists_shadow` that shadows a
durable table with the same name. The successful path proves both missing
source warnings, the temporary target is session-local, the durable shadow table
is revealed after the temporary rename, the permanent table moves durably, and
skipped targets remain absent.

Hook-build coverage adds temp-first and permanent-first variants killed at the
ownerless `dictionary-before-finish` boundary while another ownerless peer
remains live. Both variants prove the permanent target is retained, the
temporary target is absent after the crashed session exits, skipped missing
targets remain absent, the durable native file-operation marker stays set while
the peer is live, and the marker drains only after final no-live recovery.

## Affected Subsystems

- Ownerless cross-process SQL compatibility tests.
- Hook-build ownerless dictionary DDL crash selectors.
- Compatibility and ownerless concurrency documentation.

No product code, public API, directory-layout, dependency, wire-protocol, or
binary-size change is expected unless the selectors expose a recovery defect.

## Compatibility Impact

This preserves MariaDB behavior: missing durable source pairs in
`RENAME TABLE IF EXISTS` produce warnings, temporary source pairs follow the
session-local temporary routing, and durable source pairs still move native
table files. Broader randomized temporary/permanent rename permutations remain
planned.

## Database-Directory And Native Storage Impact

The selectors verify only the durable permanent table move survives outside the
crashed session. Temporary rename targets must not appear as durable SQL or
native storage state. The native file-operation marker remains tied to the real
permanent move and drains after no-live recovery.

## Test And Verification Plan

- Add production selector `temporary-mixed-if-exists-rename`.
- Add hook selector `temporary-mixed-if-exists-rename-crash`.
- Add hook selector `temporary-mixed-if-exists-rename-reverse-crash`.
- Register focused hook CTests for the two crash selectors.
- Run the production selector directly under `embedded-prod` and the matching
  `sql-case` selector under `php-embedded-prod`.
- Run the focused temporary mixed hook CTests under `ownerless-test-hooks`.
- Run `tools.ci-production-builds`, format checks, and `git diff --check`.

## Acceptance Criteria

- The production selector observes exactly two missing-source warnings, proves
  the session-local temporary target value, proves the durable shadow source is
  visible after temporary rename, proves the permanent target value, and proves
  skipped target absence.
- Both hook selectors recover the permanent target with a live peer, retain the
  native file-operation marker until peer release, drain the marker after
  no-live recovery, and preserve skipped/temporary target absence through
  ownerless/native reopen plus forced `.shm` rebuild.

## Risks And Follow-Up

- This is a deterministic temp-first and permanent-first pair, not exhaustive
  permutation testing.
- Cross-schema temporary/permanent mixes and randomized missing-source
  permutations remain planned.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, and
  external MariaDB/RQG stress remain planned.
