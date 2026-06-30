# Ownerless Compressed Key-Block Lock-Copy Option Order

## Problem

Ownerless compressed row-format crash coverage already proves plain
`ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` rebuilds and the explicit
`ALGORITHM=COPY, LOCK=EXCLUSIVE` spelling for key-block sizes 1, 2, 4, 8, and
16. MariaDB accepts `LOCK=EXCLUSIVE` and `ALGORITHM=COPY` as independent
`ALTER TABLE` option items, so the reversed
`LOCK=EXCLUSIVE, ALGORITHM=COPY` spelling should reach the same ownerless
dictionary and native file-operation recovery path.

This slice adds deterministic hook-build evidence for that reversed option
order without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5682-5693` parses table options as repeated
  `create_table_option` entries.
- `mariadb/sql/sql_yacc.yy:5844-5848` maps `ROW_FORMAT = <row_types>` into
  `Lex->create_info.row_type`.
- `mariadb/sql/sql_yacc.yy:5904-5906` maps `KEY_BLOCK_SIZE = <n>` into
  `Lex->create_info.key_block_size`.
- `mariadb/sql/sql_yacc.yy:8150-8165` accepts table options,
  `alter_algorithm_option`, and `alter_lock_option` as ordinary `ALTER TABLE`
  list items.
- `mariadb/sql/sql_yacc.yy:8210-8232` stores requested algorithm and lock
  values through `Alter_info`.

## Scope And Non-Goals

In scope:

- `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=2, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production ownerless code changes;
- exhaustive table-option order matrices;
- broader compressed DDL option combinations beyond the focused key-block
  matrix;
- external randomized DDL/RQG stress.

## Design

Add one hook-build aggregate selector:

```sh
mylite_ownerless_cross_process_sql_test \
  dictionary-compressed-row-format-key-block-lock-copy-crash
```

The selector reuses the existing compressed key-block dictionary recovery
helper five times, once for each key-block size. Each case starts from a
dynamic InnoDB table with off-page payload rows, kills the writer at the
`dictionary-before-finish` hook after the native compressed table-copy rebuild
has completed, and verifies:

- the native file-operation checkpoint marker remains set while another
  ownerless peer is live;
- ownerless recovery sees compressed row-format metadata, retained row payloads,
  and native ZBLOB page evidence;
- after peer release, no-live recovery drains the marker;
- later ownerless writes, native reopen, and forced `.shm` rebuild preserve the
  rebuilt table.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The tests record that
the reversed explicit copy-lock option order behaves like the already-covered
`ALGORITHM=COPY, LOCK=EXCLUSIVE` compressed key-block rebuild spelling for the
bounded 1, 2, 4, 8, and 16 key-block sizes.

## Directory And Native Storage Impact

The covered DDLs are native InnoDB compressed table-copy rebuilds. MyLite must
keep the native file-operation checkpoint marker durable while a peer is live
and clear it only after no-live checkpoint proof.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused reversed-order compressed key-block CTest.
- Run the adjacent compressed key-block crash selector subset.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The reversed-order aggregate selector passes for key-block sizes 1, 2, 4, 8,
  and 16.
- Live-peer recovery retains the native marker.
- No-live recovery drains the native marker.
- Ownerless/native reopen and forced `.shm` rebuild preserve compressed
  metadata, ZBLOB page evidence, and rows.

## Risks And Follow-Up

- Exhaustive compressed DDL option combinations and full randomized
  external-oracle stress remain separate completion work.
