# Ownerless View Algorithm Create And Replace Crash Recovery

## Problem

Ownerless view algorithm crash recovery now covers a focused
`ALTER ALGORITHM=TEMPTABLE VIEW` rewrite. MariaDB uses the same `view_algorithm`
grammar for `CREATE VIEW` and `CREATE OR REPLACE VIEW`, so the remaining
metadata-only view-algorithm gap includes new-view creation and replacement
rewrite boundaries.

This slice adds focused live-peer recovery evidence for:

- `CREATE ALGORITHM=TEMPTABLE VIEW ... AS ...`
- `CREATE OR REPLACE ALGORITHM=TEMPTABLE VIEW ... AS ...`

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:2717` accepts
  `create_or_replace view_algorithm definer_opt opt_view_suid VIEW_SYM`.
- `mariadb/sql/sql_yacc.yy:7524` accepts
  `ALTER view_algorithm definer_opt opt_view_suid VIEW_SYM`.
- `mariadb/sql/sql_yacc.yy:18447` defines `view_algorithm` as
  `ALGORITHM = UNDEFINED|MERGE|TEMPTABLE`.
- `mariadb/sql/sql_view.cc:841` persists `algorithm` in the native view
  metadata file, and `mariadb/sql/sql_view.cc:1071` stores the selected
  algorithm on the view object.

## Design

Add two hook-build selectors:

- `dictionary-view-algorithm-create-crash`
- `dictionary-view-algorithm-replace-crash`

Each selector kills a child writer at the existing `dictionary-before-finish`
fault after MariaDB has persisted native view metadata and before MyLite
publishes ownerless dictionary finish. The parent keeps another ownerless peer
live, verifies recovery with the native file-operation marker clear, releases
the peer, and verifies ownerless/native reopen before and after forced
shared-memory rebuild.

The existing view recovery classifier already consumes the optional
`ALGORITHM = ...` prefix for `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and
`ALTER VIEW`; this slice adds the missing create/replacement evidence rather
than a new classifier branch.

## Compatibility Impact

No SQL syntax or public API changes. The slice broadens ownerless recovery
evidence for MariaDB-compatible view algorithm clauses so killed metadata-only
view create and replacement rewrites can recover with a live peer.

## Storage And Lifecycle Impact

The covered native file is the view `.frm` metadata file inside `datadir/app/`.
These are metadata-only view DDL operations, so the native file-operation
checkpoint marker must remain clear.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused create, replacement, and alter algorithm selectors.
- Run the adjacent view crash selector group.
- Run relevant production embedded view/ownerless peer-refresh cases.
- Run `tools/check-ci-production-builds`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- A killed `CREATE ALGORITHM=TEMPTABLE VIEW` writer recovers while another
  ownerless peer remains live.
- A killed `CREATE OR REPLACE ALGORITHM=TEMPTABLE VIEW` writer recovers while
  another ownerless peer remains live.
- `INFORMATION_SCHEMA.VIEWS.ALGORITHM` reports `TEMPTABLE` after recovery.
- The recovered views expose the rewritten non-updatable projection and reject
  the old exposed column names where applicable.
- The native file-operation marker remains clear.
- Ownerless/native reopen and forced `.shm` rebuild preserve the recovered view
  metadata and base-table durability.

## Non-Goals

- Exhaustive algorithm/security/check-option ordering matrices.
- Invalid definer or privilege matrices.
- Stored routine execution support.
- External MariaDB/RQG randomized stress.
