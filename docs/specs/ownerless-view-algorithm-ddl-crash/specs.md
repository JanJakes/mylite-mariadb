# Ownerless View Algorithm DDL Crash Recovery

## Problem

Ownerless view crash coverage proves simple create/drop, replacement/alter,
column-list, check-option, nested check-option, security, idempotent/no-op, and
invalid-dependency drop metadata-only boundaries. The remaining broader view
gap still includes accepted MariaDB metadata clauses that change the stored
view definition without touching native table files.

This slice adds focused live-peer recovery evidence for an `ALTER VIEW` that
changes the stored view algorithm from `MERGE` to `TEMPTABLE`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:18447` accepts `ALGORITHM=UNDEFINED`,
  `ALGORITHM=MERGE`, and `ALGORITHM=TEMPTABLE` as `view_algorithm`.
- `mariadb/sql/sql_yacc.yy:2717` accepts `CREATE ... view_algorithm ... VIEW`;
  `mariadb/sql/sql_yacc.yy:7524` accepts
  `ALTER view_algorithm ... VIEW`.
- `mariadb/sql/sql_view.cc:841` persists `algorithm` in the view metadata
  file, and `mariadb/sql/sql_view.cc:1071` stores the selected algorithm on
  the view object during view creation or rewrite.
- `mariadb/sql/sql_show.cc:7637` exposes the view algorithm through
  `INFORMATION_SCHEMA.VIEWS`.

## Design

Add the unsafe-hook selector `dictionary-view-algorithm-alter-crash`.

The test creates an InnoDB base table and an initial:

```sql
CREATE ALGORITHM=MERGE VIEW app.ownerless_view_algorithm_alter_crash AS ...
```

Then a child writer runs:

```sql
ALTER ALGORITHM=TEMPTABLE VIEW app.ownerless_view_algorithm_alter_crash AS ...
```

with the existing `dictionary-before-finish` fault armed. The parent keeps a
second ownerless peer live, verifies recovery with the native file-operation
marker clear, releases the peer, and verifies ownerless/native reopen before
and after forced shared-memory rebuild.

## Compatibility Impact

No SQL syntax or public API changes. The slice broadens ownerless recovery
classification for MariaDB-compatible view algorithm clauses so a killed
metadata-only view rewrite can recover with a live peer instead of remaining
busy as an unknown dictionary operation.

## Storage And Lifecycle Impact

The covered native file is the view `.frm` metadata file inside
`datadir/app/`. This is metadata-only view DDL, so the native file-operation
checkpoint marker must remain clear.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-view-algorithm-alter-crash$' --output-on-failure`.
- Run adjacent view crash selectors.
- Run relevant production embedded view/ownerless peer-refresh cases.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- A killed `ALTER ALGORITHM=TEMPTABLE VIEW` writer recovers while another
  ownerless peer remains live.
- `INFORMATION_SCHEMA.VIEWS.ALGORITHM` reports `TEMPTABLE` after recovery.
- The rewritten view columns and result set are visible through ownerless and
  ordinary native reopen.
- The native file-operation marker remains clear before and after live-peer
  recovery.
- Forced `.shm` rebuild preserves the recovered view metadata.

## Non-Goals

- Exhaustive view algorithm/security/check-option permutations.
- Invalid definer or privilege matrices.
- Stored routine execution support.
- External MariaDB/RQG randomized stress.
