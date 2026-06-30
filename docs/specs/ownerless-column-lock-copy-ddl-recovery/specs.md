# Ownerless Column Lock-Copy DDL Recovery

## Problem

Ownerless column ALTER crash recovery already covers plain column ADD and the
explicit `ALGORITHM=COPY, LOCK=EXCLUSIVE` ADD-column spelling at the native
dictionary completion boundary. MariaDB also accepts the same requested lock and
algorithm as separate `ALTER TABLE` option items in the opposite order. MyLite
needs evidence that this supported SQL spelling reaches the same ownerless
dictionary finish and native file-lifecycle recovery path.

This slice adds deterministic hook-build evidence for the reversed
`LOCK=EXCLUSIVE, ALGORITHM=COPY` ADD-column spelling without changing
production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:7965-7974` parses `ALTER TABLE` as an
  `alter_list` of comma-separated `alter_list_item` entries.
- `mariadb/sql/sql_yacc.yy:8160-8171` accepts `alter_algorithm_option` and
  `alter_lock_option` as ordinary `alter_list_item` alternatives, so
  `ADD COLUMN ..., LOCK=EXCLUSIVE, ALGORITHM=COPY` and
  `ADD COLUMN ..., ALGORITHM=COPY, LOCK=EXCLUSIVE` are both valid list orders.
- `mariadb/sql/sql_yacc.yy:8211-8229` stores the requested algorithm and lock
  values through `Alter_info`.
- `mariadb/sql/sql_alter.cc:62-80` and `mariadb/sql/sql_alter.cc:84-99` map
  the text values `COPY` and `EXCLUSIVE` to `ALTER_TABLE_ALGORITHM_COPY` and
  `ALTER_TABLE_LOCK_EXCLUSIVE`.
- MyLite's ownerless hook selector already kills column copy-lock DDL after
  native metadata completion and before ownerless dictionary finish. This slice
  adds the corresponding reversed-order selector.

## Scope And Non-Goals

In scope:

- `ALTER TABLE app.<table> ADD COLUMN note INT NOT NULL DEFAULT 7,
  LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- kill after MariaDB/InnoDB native metadata update or copy rebuild and before
  ownerless dictionary finish;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production code changes;
- exhaustive option-order matrices for every column ALTER action;
- randomized DDL/RQG stress.

## Design

Add a hook-build direct selector:

```sh
mylite_ownerless_cross_process_sql_test dictionary-column-add-lock-copy-crash
```

The selector creates an InnoDB table, verifies the `note` column is absent,
kills a child at the `dictionary-before-finish` hook while executing the
reversed-order copy-lock ADD, and then verifies:

- a live ownerless peer prevents native file-operation marker drain;
- ownerless reopen sees the recovered column metadata and default values;
- a row inserted after recovery gets the default `note` value;
- after the live peer exits, no-live ownerless close drains the native
  file-operation marker;
- native reopen and forced `.shm` rebuild preserve metadata and rows.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The test records that
MyLite ownerless recovery treats MariaDB's reversed explicit copy-lock option
order the same as the already-covered copy-lock order.

## Directory And Native Storage Impact

The covered DDL is a native InnoDB copy-style table rebuild. The post-crash
oracle requires MyLite to keep the native file-operation checkpoint marker set
while another ownerless peer is live and to clear it only after no-live recovery
has proven the native table state durable.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-dictionary-column-add-lock-copy-crash$'
  --output-on-failure`.
- Run adjacent ownerless column ADD crash selectors.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Reversed lock/algorithm option order reaches the native dictionary completion
  boundary and recovers the added column.
- Live-peer recovery keeps the native file-operation marker set.
- No-live recovery drains the marker.
- Ownerless reopen, native reopen, and forced `.shm` rebuild all preserve the
  recovered column metadata and defaulted rows.

## Risks And Follow-Up

- This closes one supported SQL option-order boundary for column ADD. Other
  column ALTER actions and longer randomized DDL/file-lifecycle stress remain
  separate completion work.
