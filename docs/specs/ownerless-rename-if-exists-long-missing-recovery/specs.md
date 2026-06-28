# Ownerless Rename IF EXISTS Long Missing Recovery

## Problem Statement

Ownerless `RENAME TABLE IF EXISTS` recovery already covers focused
missing-source/existing/missing-source lists for same-schema and cross-schema
renames. That proves the optional `IF EXISTS` syntax, skipped-source warning
behavior, live-peer recovery, and first native rename-pair DDL-log rollback.
It does not prove a longer supported list where multiple missing source pairs
are interleaved with multiple existing InnoDB table moves.

This slice adds a bounded same-schema longer permutation:

```sql
RENAME TABLE IF EXISTS
  app.missing_a TO app.missing_a_dst,
  app.left_src TO app.left_dst,
  app.missing_b TO app.missing_b_dst,
  app.right_src TO app.right_dst,
  app.missing_c TO app.missing_c_dst
```

It proves MariaDB warning/no-op semantics for the three skipped sources, ownerless
live-peer recovery after both real native renames complete, and native DDL-log
rollback after the second real rename pair.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8713-8725` parses
  `RENAME table_or_tables opt_if_exists ...` and records the option in
  `LEX::create_info`.
- `mariadb/sql/sql_parse.cc:4397-4405` handles `SQLCOM_RENAME_TABLE`,
  propagates session `OPTION_IF_EXISTS`, and calls
  `mysql_rename_tables(..., lex->if_exists())`.
- `mariadb/sql/sql_rename.cc:56-181` implements `mysql_rename_tables()`,
  locks the ordered rename list, and calls `rename_tables()`.
- `mariadb/sql/sql_rename.cc:506-674` implements the ordered pair loop:
  non-temporary pairs call `check_rename(..., skip_error || if_exists)`;
  missing sources return `< 0` and are skipped under `IF EXISTS`, while real
  pairs call `do_rename()`.
- `mariadb/sql/sql_table.cc:5562-5705` performs native handler and `.frm`
  rename work through `mysql_rename_table()`, including DDL-log backup records.

## Scope And Non-Goals

In scope:

- Same-schema longer `RENAME TABLE IF EXISTS` with three missing sources and
  two real InnoDB table moves.
- Production ownerless SQL coverage for MariaDB warning/no-op behavior and
  final target persistence through ownerless/native reopen and forced `.shm`
  rebuild.
- Hook-build live-peer crash coverage at `dictionary-before-finish`.
- Hook-build native-loop crash coverage at the second
  `rename-table-after-native-file-op` hook hit.

Out of scope:

- Target-conflict `RENAME TABLE IF EXISTS` reachability. Prior SQL-shape
  investigations showed some SQL forms stop before ownerless callbacks; this
  slice only claims the supported reachable longer missing-source path.
- Temporary/permanent mixed `IF EXISTS` rename lists.
- Cross-schema longer missing-source lists beyond the focused
  missing/existing/missing cross-schema matrix already covered.
- Foreign-key longer missing-source permutations.

## Design

No product runtime change is intended. The slice extends
`mylite_ownerless_cross_process_sql_test` with:

- a normal selector, `rename-if-exists-long-missing-noop`, that verifies three
  MariaDB note `1146` warnings, two completed target tables, and absence of all
  skipped targets;
- `dictionary-rename-if-exists-long-missing-crash`, which kills a writer after
  native file work and before ownerless dictionary finish while another
  ownerless process remains live;
- `dictionary-rename-if-exists-long-missing-loop-crash`, which uses the existing
  unsafe ownerless fault skip mechanism to kill after the second real native
  rename-pair hook and verifies MariaDB DDL-log rollback to both original source
  table names and `SPACE` ids.

## Compatibility Impact

Successful SQL behavior remains MariaDB-owned. The covered behavior is that
missing source pairs under `RENAME TABLE IF EXISTS` produce note diagnostics
and do not create target tables, while existing InnoDB source pairs still move
through MariaDB's ordered native rename loop. Ownerless recovery evidence now
covers a longer interleaving of those MariaDB-supported outcomes.

## Directory, Lifecycle, And Native Storage Impact

No directory-layout or native storage format change. The tests verify native
`.frm` and `.ibd` files stay inside the MyLite database directory, skipped
targets are not materialized, real target tables survive completed recovery, and
native-loop rollback restores the original source files plus InnoDB `SPACE`
identity through ownerless/native reopen and forced shared-memory rebuild.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The slice adds focused test harness code, CTest registrations, and docs.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run direct selectors:
  `rename-if-exists-long-missing-noop`,
  `dictionary-rename-if-exists-long-missing-crash`, and
  `dictionary-rename-if-exists-long-missing-loop-crash`.
- Run adjacent hook CTests for same-schema `RENAME TABLE IF EXISTS` missing
  source crash coverage.
- Build and run the production selector under `embedded-prod`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Longer `RENAME TABLE IF EXISTS` emits exactly the expected skipped-source
  warnings and leaves all skipped targets absent.
- Both real target tables are recovered and writable after a killed writer at
  `dictionary-before-finish` while a live peer keeps no-live cleanup deferred.
- Killing after the second real native rename-pair hook rolls back both source
  tables to their original names and original InnoDB `SPACE` ids.
- Marker retention, final no-live marker drain, ownerless/native reopen, and
  forced `.shm` rebuild all preserve the expected state.

## Risks And Follow-Up

- Target-conflict, temporary/permanent, FK, and cross-schema longer permutations
  remain separate DDL/file-lifecycle follow-ups.
- This slice is bounded evidence for a reachable supported SQL path; it does
  not claim arbitrary SQL-level table-lock callback reachability.
