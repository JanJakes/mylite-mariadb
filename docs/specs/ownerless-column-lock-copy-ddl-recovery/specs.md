# Ownerless Column Lock-Copy DDL Recovery

## Problem

Ownerless column ALTER crash recovery already covers plain column ADD and the
explicit `ALGORITHM=COPY, LOCK=EXCLUSIVE` ADD-column spelling at the native
dictionary completion boundary. Later coverage added the reversed unplaced
ADD-column spelling, but the placed ADD, DROP, MODIFY, CHANGE, and RENAME
table-copy forms still lacked the opposite option-order proof. MariaDB accepts
the requested lock and algorithm as separate `ALTER TABLE` option items in
either order. MyLite needs evidence that these supported SQL spellings reach
the same ownerless dictionary finish and native file-lifecycle recovery path.

This slice adds deterministic hook-build evidence for the remaining reversed
`LOCK=EXCLUSIVE, ALGORITHM=COPY` column table-copy spellings without changing
production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:7965-7974` parses `ALTER TABLE` as an
  `alter_list` of comma-separated `alter_list_item` entries.
- `mariadb/sql/sql_yacc.yy:8160-8171` accepts `alter_algorithm_option` and
  `alter_lock_option` as ordinary `alter_list_item` alternatives, so the
  focused ADD, DROP, MODIFY, CHANGE, and RENAME column clauses can be followed
  by `LOCK=EXCLUSIVE, ALGORITHM=COPY` or
  `ALGORITHM=COPY, LOCK=EXCLUSIVE`.
- `mariadb/sql/sql_yacc.yy:8211-8229` stores the requested algorithm and lock
  values through `Alter_info`.
- `mariadb/sql/sql_alter.cc:62-80` and `mariadb/sql/sql_alter.cc:84-99` map
  the text values `COPY` and `EXCLUSIVE` to `ALTER_TABLE_ALGORITHM_COPY` and
  `ALTER_TABLE_LOCK_EXCLUSIVE`.
- MyLite's ownerless hook selectors already kill column copy-lock DDL after
  native metadata completion and before ownerless dictionary finish. This slice
  keeps those oracles and adds a corresponding reversed-order aggregate
  selector for placed ADD, DROP, MODIFY, CHANGE, and RENAME.

## Scope And Non-Goals

In scope:

- `ALTER TABLE app.<table> ADD COLUMN <column> ... FIRST/AFTER <column>,
  LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE app.<table> DROP COLUMN note, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- `ALTER TABLE app.<table> MODIFY COLUMN note VARCHAR(32) NOT NULL DEFAULT
  'changed', LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE app.<table> CHANGE COLUMN note changed_note VARCHAR(32) NOT
  NULL DEFAULT 'changed', LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- `ALTER TABLE app.<table> RENAME COLUMN note TO renamed_note,
  LOCK=EXCLUSIVE, ALGORITHM=COPY`;
- kill after MariaDB/InnoDB native metadata update or copy rebuild and before
  ownerless dictionary finish;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production code changes;
- exhaustive option-order matrices beyond these focused table-copy column
  ALTER actions;
- randomized DDL/RQG stress.

## Design

Add a hook-build aggregate selector:

```sh
mylite_ownerless_cross_process_sql_test dictionary-column-copy-lock-option-order-crash
```

The selector reuses the existing copy-lock column crash oracles against
isolated database directories, but swaps the fault callbacks to execute the
reversed option order. It kills a child at the `dictionary-before-finish` hook
for placed ADD, DROP, MODIFY, CHANGE, and RENAME, and then verifies:

- a live ownerless peer prevents native file-operation marker drain;
- ownerless reopen sees recovered column order, absent dropped-column metadata,
  modified-column width/default metadata, changed-column name/default metadata,
  and renamed-column metadata;
- rows inserted after recovery observe the new defaults or widened column
  definitions;
- after the live peer exits, no-live ownerless close drains the native
  file-operation marker;
- native reopen and forced `.shm` rebuild preserve metadata and rows.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The tests record that
MyLite ownerless recovery treats MariaDB's reversed explicit copy-lock option
order the same as the already-covered copy-lock order for the focused column
table-copy ALTER classes.

## Directory And Native Storage Impact

The covered DDL statements are native InnoDB copy-style table rebuilds. The
post-crash oracles require MyLite to keep the native file-operation checkpoint
marker set while another ownerless peer is live and to clear it only after
no-live recovery has proven the native table state durable.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-dictionary-column-copy-lock-option-order-crash$'
  --output-on-failure`.
- Run adjacent ownerless column copy-lock crash selectors.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- Reversed lock/algorithm option order reaches the native dictionary completion
  boundary for placed ADD, DROP, MODIFY, CHANGE, and RENAME.
- Live-peer recovery keeps the native file-operation marker set.
- No-live recovery drains the marker.
- Ownerless reopen, native reopen, and forced `.shm` rebuild all preserve the
  recovered column metadata and rows.

## Risks And Follow-Up

- This closes focused explicit option-order proof for the covered column
  table-copy classes. Longer randomized DDL/file-lifecycle stress and broader
  multi-action column ALTER matrices remain separate completion work.
