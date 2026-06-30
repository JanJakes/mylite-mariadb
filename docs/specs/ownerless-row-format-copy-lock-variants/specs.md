# Ownerless Row-Format Copy-Lock Variants

## Problem

Ownerless row-format rebuild crash recovery covers plain
`ROW_FORMAT=COMPACT`, plain `ROW_FORMAT=REDUNDANT`, and both plain and explicit
copy-lock `ROW_FORMAT=DYNAMIC` rebuilds. Compact and redundant rebuilds still
need evidence for the explicit copy-lock spelling that MariaDB accepts on the
same `ALTER TABLE` statement.

This slice adds hook-build coverage for compact and redundant
`ALGORITHM=COPY, LOCK=EXCLUSIVE` row-format rebuilds without changing
production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5682-5693` parses table options as repeated
  `create_table_option` entries.
- `mariadb/sql/sql_yacc.yy:5844-5848` maps `ROW_FORMAT = <row_types>` into
  `Lex->create_info.row_type` and marks `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_yacc.yy:6041-6048` includes `COMPACT` and `REDUNDANT` row
  types.
- `mariadb/sql/sql_yacc.yy:8150-8165` accepts table options,
  `alter_algorithm_option`, and `alter_lock_option` as `ALTER TABLE`
  `alter_list_item` entries.
- `mariadb/sql/sql_yacc.yy:8210-8232` stores requested algorithm and lock
  values through `Alter_info`.

## Scope And Non-Goals

In scope:

- `ALTER TABLE app.<table> ROW_FORMAT=COMPACT, ALGORITHM=COPY, LOCK=EXCLUSIVE`;
- `ALTER TABLE app.<table> ROW_FORMAT=REDUNDANT, ALGORITHM=COPY, LOCK=EXCLUSIVE`;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production ownerless code changes;
- reversed lock/algorithm spelling for these row-format variants;
- exhaustive row-format/table-option matrices;
- external randomized DDL/RQG stress.

## Design

Add two hook-build direct selectors:

```sh
mylite_ownerless_cross_process_sql_test dictionary-row-format-compact-copy-lock-crash
mylite_ownerless_cross_process_sql_test dictionary-row-format-redundant-copy-lock-crash
```

Both selectors reuse the existing row-format dictionary recovery helper. Each
starts from a dynamic InnoDB table, kills the writer at the
`dictionary-before-finish` hook after the native copy rebuild has completed,
and verifies:

- the native file-operation checkpoint marker is set while another ownerless
  peer remains live;
- ownerless recovery sees the expected compact or redundant row-format metadata
  and retained rows;
- after peer release, no-live recovery drains the native marker;
- later ownerless writes, native reopen, and forced `.shm` rebuild preserve the
  rebuilt table.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The tests record that
explicit copy-lock compact and redundant row-format rebuilds use the same
ownerless recovery path as the already-covered plain compact/redundant and
dynamic copy-lock row-format rebuilds.

## Directory And Native Storage Impact

The covered DDLs are native InnoDB table-copy rebuilds. The recovery oracle
requires the native file-operation checkpoint marker to remain durable while a
peer is live and to clear only after no-live checkpoint proof.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the two focused CTests for compact and redundant copy-lock row-format
  rebuilds.
- Run the adjacent row-format crash selector subset.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Both explicit copy-lock row-format selectors pass.
- Live-peer recovery retains the native marker.
- No-live recovery drains the native marker.
- Ownerless/native reopen and forced `.shm` rebuild preserve metadata and rows.

## Risks And Follow-Up

- Reversed `LOCK=EXCLUSIVE, ALGORITHM=COPY` spelling and broader table-option
  combinations remain separate DDL/file-lifecycle evidence work.
