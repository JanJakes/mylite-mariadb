# Ownerless Rebuild Lock-Copy Option Order

## Problem

Ownerless DDL crash recovery covers table charset conversion and dynamic,
compact, and redundant row-format rebuilds with the explicit
`ALGORITHM=COPY, LOCK=EXCLUSIVE` spelling. MariaDB accepts `LOCK=EXCLUSIVE` and
`ALGORITHM=COPY` as independent `ALTER TABLE` option items, so the reversed
`LOCK=EXCLUSIVE, ALGORITHM=COPY` spelling should reach the same ownerless
dictionary and native file-operation recovery path.

This slice adds deterministic hook-build evidence for those reversed
option-order rebuild forms without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5682-5693` parses table options as repeated
  `create_table_option` entries.
- `mariadb/sql/sql_yacc.yy:5844-5848` maps `ROW_FORMAT = <row_types>` into
  `Lex->create_info.row_type`.
- `mariadb/sql/sql_yacc.yy:6041-6048` includes `DYNAMIC`, `COMPACT`, and
  `REDUNDANT` row types.
- `mariadb/sql/sql_yacc.yy:8139-8146` handles
  `CONVERT TO CHARACTER SET ... [COLLATE ...]` as an ALTER list item.
- `mariadb/sql/sql_yacc.yy:8150-8165` accepts table options,
  `alter_algorithm_option`, and `alter_lock_option` as ordinary ALTER list
  items.
- `mariadb/sql/sql_yacc.yy:8210-8232` stores requested algorithm and lock
  values through `Alter_info`.

## Scope And Non-Goals

In scope:

- `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...,
  LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=DYNAMIC, LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=COMPACT, LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=REDUNDANT, LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production ownerless code changes;
- compressed key-block reversed option-order coverage;
- exhaustive table-option order matrices;
- external randomized DDL/RQG stress.

## Design

Add four hook-build direct selectors:

```sh
mylite_ownerless_cross_process_sql_test dictionary-charset-convert-lock-copy-crash
mylite_ownerless_cross_process_sql_test dictionary-row-format-lock-copy-crash
mylite_ownerless_cross_process_sql_test dictionary-row-format-compact-lock-copy-crash
mylite_ownerless_cross_process_sql_test dictionary-row-format-redundant-lock-copy-crash
```

Each selector reuses the existing charset-conversion or row-format dictionary
recovery helper, kills the writer at the `dictionary-before-finish` hook after
the native rebuild has completed, and verifies:

- the native file-operation checkpoint marker remains set while another
  ownerless peer is live;
- ownerless recovery sees expected charset/collation or row-format metadata
  and retained rows;
- after peer release, no-live recovery drains the marker;
- later ownerless writes, native reopen, and forced `.shm` rebuild preserve the
  rebuilt table.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The tests record that
the reversed explicit copy-lock option order behaves like the already-covered
`ALGORITHM=COPY, LOCK=EXCLUSIVE` rebuild spellings for these supported ALTER
forms.

## Directory And Native Storage Impact

The covered DDLs are native InnoDB table-copy rebuilds. MyLite must keep the
native file-operation checkpoint marker durable while a peer is live and clear
it only after no-live checkpoint proof.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the four focused reversed-order CTests.
- Run the adjacent charset and row-format crash selector subset.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- All four reversed option-order selectors pass.
- Live-peer recovery retains the native marker.
- No-live recovery drains the native marker.
- Ownerless/native reopen and forced `.shm` rebuild preserve metadata and rows.

## Risks And Follow-Up

- Compressed key-block reversed option-order coverage and longer randomized
  external DDL oracles remain separate follow-up work.
