# Ownerless CREATE OR REPLACE After-Drop Crash

## Problem Statement

Ownerless `CREATE OR REPLACE TABLE` crash coverage already kills a writer after
MariaDB has removed the old table and created the replacement table but before
MyLite publishes ownerless dictionary finish. One lower native DDL lifecycle
window remained unproven: the writer can die after MariaDB removes the old
target table and before the replacement `.frm` and engine table are created.

This slice adds a narrow unsafe hook at that MariaDB after-drop boundary and
focused ownerless recovery coverage for the durable cleanup result: the old
table remains absent, no half-created replacement is exposed, and the same SQL
name can be cleanly reused after no-live ownerless recovery.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:4771` through
  `mariadb/sql/sql_table.cc:4784` detects an existing normal table during
  `create_table_impl()` and enters the `options.or_replace()` path.
- `mariadb/sql/sql_table.cc:4806` through
  `mariadb/sql/sql_table.cc:4810` removes the old normal table through
  `mysql_rm_table_no_locks()` while keeping table locks.
- `mariadb/sql/sql_table.cc:4812` already has MariaDB's
  `debug_crash_here("ddl_log_create_after_drop")` boundary immediately after
  that removal. The unsafe ownerless hook added here fires at the same source
  point.
- `mariadb/sql/sql_table.cc:4944` through
  `mariadb/sql/sql_table.cc:4954` is the later `.frm`/engine replacement
  creation path. The new hook fires before this block.
- `packages/libmylite/src/database.cc:14147` through
  `packages/libmylite/src/database.cc:14167` begins ownerless dictionary DDL
  before native SQL execution, so a crash in this native after-drop window
  leaves the dictionary owner active until no-live recovery.
- `packages/libmylite/src/database.cc:14171` through
  `packages/libmylite/src/database.cc:14177` remains the existing later
  dictionary-finish crash boundary for completed native DDL.

## Design

Add a single MariaDB SQL hook call:

- declare `mylite_ownerless_innodb_test_fault(const char *)` in
  `mariadb/sql/sql_table.cc`,
- call it with `create-or-replace-after-drop` immediately after
  `debug_crash_here("ddl_log_create_after_drop")`.

The existing ownerless hook build enables that fault primitive only when
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS` is active. Normal production runs
do not configure the fault and do not pause. The hook is intentionally placed
at an existing upstream debug-crash point to minimize the MariaDB fork delta.

Add a hook-only selector,
`dictionary-create-or-replace-after-drop-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates an original InnoDB file-per-table table with old columns, old rows,
  and an old secondary index,
- starts a live ownerless peer,
- kills a writer at `create-or-replace-after-drop` while the ownerless
  dictionary DDL owner is active,
- verifies the live peer prevents recovery cleanup until no live ownerless
  peers remain,
- verifies ownerless and ordinary native reopen see the same absent-table state
  before and after forced `.shm` rebuild,
- recreates the same SQL table name and verifies ownerless/native reopen see
  the recreated table, rows, replacement index, and old metadata absence before
  and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- representative plain `CREATE OR REPLACE TABLE` over an existing InnoDB
  file-per-table table,
- crash after old target removal and before replacement native creation,
- absent-table recovery and same-name reuse,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- separate after-drop selectors for `CREATE OR REPLACE TABLE ... LIKE` and
  `CREATE OR REPLACE TABLE ... AS SELECT`,
- foreign-key, trigger, generated-column, partition, special-index, or
  unsupported-storage-option replacement variants,
- changing MariaDB's `CREATE OR REPLACE TABLE` failure semantics,
- external MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL surface is newly enabled. The slice strengthens existing ownerless
`CREATE OR REPLACE TABLE` evidence by proving the earlier native after-drop
crash window does not resurrect stale table files, stale dictionary metadata,
or retained ownerless page-version state.

## DDL Metadata Routing Impact

The selector verifies table absence through `INFORMATION_SCHEMA.TABLES`, SQL
execution failure for the removed table, `.frm`/`.ibd` absence in the MyLite
database directory, and later recreated column/index metadata through
`INFORMATION_SCHEMA.COLUMNS` and `INFORMATION_SCHEMA.STATISTICS`.

## Directory And Lifecycle Impact

No directory layout changes. The test observes the existing
`datadir/app/<table>.frm`, `datadir/app/<table>.ibd`,
`concurrency/mylite-concurrency.shm`, and ownerless no-live recovery lifecycle.

## Native Storage Impact

No storage format changes. MariaDB native removal remains the authority for the
after-drop failure state. The hook proves MyLite ownerless recovery does not
overlay older retained pages or metadata onto a removed same-name tablespace.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
MariaDB fork delta is one test-fault declaration and one hook call at an
existing debug-crash boundary.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-after-drop-crash`
- Run adjacent hook selectors:
  - `dictionary-create-or-replace-table-crash`
  - `dictionary-create-or-replace-like-crash`
  - `dictionary-create-or-replace-ctas-crash`
- Build the production SQL test target and run the normal
  `table-idempotent-ddl` selector.
- Run the hook ownerless SQL CTest subset containing negative proof and hook
  crash coverage.
- Run the production ownerless SQL weighted shards when timing allows.
- Run format and whitespace checks.

## Acceptance Criteria

- The focused selector reaches the new after-drop fault and does not hang.
- A live peer prevents cleanup until no-live ownerless recovery.
- Reopen after the crash sees no `.frm`, no `.ibd`, no
  `INFORMATION_SCHEMA.TABLES` row, and SQL access to the removed table fails.
- Forced `.shm` rebuild preserves the absent-table state.
- The same table name can be recreated after recovery.
- Ownerless and ordinary native reopen observe the recreated table metadata,
  rows, index, and old metadata absence before and after forced `.shm` rebuild.

## Evidence

Focused and adjacent hook selectors passed:

```text
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-after-drop-crash
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-table-crash
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-like-crash
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-ctas-crash
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_crashed_create_or_replace_after_drop_dictionary_ddl_recovers_absence
```

Production and hook CTest coverage passed:

```text
build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test table-idempotent-ddl
ctest --preset ownerless-test-hooks -R '^libmylite\.(ownerless-negative-proof|embedded-ownerless-innodb-lock-hooks)$' --output-on-failure
ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.15$' --output-on-failure
ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.(8|11)$' --output-on-failure
ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure
```

The full production ownerless SQL shard run passed 13 shards, then shard 15
timed out in unrelated
`test_ownerless_index_idempotent_ddl_refreshes_peer_dictionary`; that case
passed directly in 3 seconds, shard 15 passed isolated in 26.58 seconds, and
the two serial shards 8 and 11 passed isolated.

Static checks passed:

```text
cmake --build --preset format-check-prod
git diff --check
tools/check-ci-production-builds
```

## Risks And Unresolved Questions

- The hook uses the existing InnoDB ownerless test-fault primitive from SQL
  code. It is intentionally confined to an unsafe hook build and a source point
  that already has an upstream MariaDB debug-crash marker.
- This proves one representative after-drop replacement failure. Broader copy
  variants, constraint-heavy replacements, and external randomized DDL stress
  remain separate coverage targets.
