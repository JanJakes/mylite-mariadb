# Ownerless Schema Idempotent DDL Crash

## Problem

Ownerless schema-idempotent DDL coverage verifies peer refresh for
`CREATE SCHEMA IF NOT EXISTS`, duplicate `CREATE DATABASE IF NOT EXISTS`, and
`DROP SCHEMA IF EXISTS`. The remaining bounded crash boundary is a writer
killed after MariaDB completes duplicate-create or missing-drop no-op schema DDL
but before MyLite publishes ownerless dictionary finish.

This slice adds hook-build live-peer recovery evidence for duplicate and
missing idempotent schema create/drop boundaries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/lex.h` maps `SCHEMA` to the `DATABASE` token, so `SCHEMA` and
  `DATABASE` spellings share the same parser command family.
- `mariadb/sql/sql_yacc.yy` parses `CREATE DATABASE` with
  `opt_if_not_exists` and `DROP DATABASE` with `opt_if_exists`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_DB` through
  `mysql_create_db()` and `SQLCOM_DROP_DB` through `mysql_rm_db()`.
- `mariadb/sql/sql_db.cc` handles an existing schema with `IF NOT EXISTS` as a
  successful note/no-op path without rewriting the existing `db.opt` defaults.
- `mariadb/sql/sql_db.cc` handles a missing schema under `DROP DATABASE IF
  EXISTS` as a successful note/no-op path without table-file lifecycle work.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `DROP` as
  ownerless dictionary DDL and exposes the unsafe `dictionary-before-finish`
  hook after native SQL execution but before ownerless dictionary finish.

## Design

Add distinct recoverable dictionary kinds for the no-op statement shapes:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA_IF_NOT_EXISTS`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA_IF_EXISTS`

Classify bounded `CREATE DATABASE|SCHEMA IF NOT EXISTS <identifier>` with
default charset/collation options and bounded `DROP DATABASE|SCHEMA IF EXISTS
<identifier>` into those kinds after native SQL success. Register both as
metadata-only recovery. Also register `DROP_SCHEMA_IF_EXISTS` in the native
file-operation lane so the same SQL spelling can safely recover through marker
evidence when an existing-schema variant performs native file removals.

Promote the existing unsafe-hook selectors in
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-schema-idempotent-create-crash` creates a schema with explicit
  `latin1` defaults and an InnoDB table, then kills a writer after duplicate
  `CREATE DATABASE IF NOT EXISTS` with different `utf8mb4` defaults reaches
  `dictionary-before-finish`.
- `dictionary-schema-idempotent-drop-crash` creates a separate schema and
  InnoDB table, then kills a writer after `DROP SCHEMA IF EXISTS` for a
  missing schema name reaches `dictionary-before-finish`.
- `dictionary-schema-idempotent-missing-create-crash` kills a writer after
  `CREATE SCHEMA IF NOT EXISTS` creates a missing schema and native `db.opt`
  reaches `dictionary-before-finish`.
- `dictionary-schema-idempotent-existing-drop-crash` kills a writer after
  `DROP DATABASE IF EXISTS` removes a table-bearing schema and reaches
  `dictionary-before-finish`.
- `dictionary-schema-idempotent-empty-drop-crash` kills a writer after
  `DROP DATABASE IF EXISTS` removes an empty existing schema and reaches
  `dictionary-before-finish`.

The no-op, missing-create, and empty-drop selectors keep a live ownerless peer
open while the writer is killed, recover through a new ownerless opener while
that peer remains live with the native file-operation marker clear, then verify
ownerless and ordinary native reopen before and after forced `.shm` rebuild.
The table-bearing existing-drop selector keeps the native file-operation marker
set while a peer remains live and drains it only after final no-live recovery.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate
  `CREATE DATABASE IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing
  `DROP SCHEMA IF EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing
  `CREATE SCHEMA IF NOT EXISTS` create behavior.
- Crash-at-`dictionary-before-finish` coverage for existing
  `DROP DATABASE IF EXISTS` table-bearing drop behavior.
- Crash-at-`dictionary-before-finish` coverage for existing
  `DROP DATABASE IF EXISTS` empty-schema drop behavior.
- Native schema directory and `db.opt` preservation for the real schema.
- Absence of missing-schema native directories and metadata after no-op drop
  recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE DATABASE`.
- Crash injection inside native `db.opt` write or schema directory creation.
- `DROP SCHEMA IF EXISTS` existing-schema synonym crash recovery beyond the
  `DROP DATABASE IF EXISTS` spellings.
- SQL-level table-lock fault injection for native table-wait paths.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent schema migration statements by proving a dead
writer in MyLite's dictionary publication window does not rewrite existing
schema defaults, drop the real schema, create the missing schema, or leave
stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native schema directories and
`db.opt` files under `datadir/`, ownerless live-peer recovery with the native
file-operation marker clear for no-op/create cases, native file-operation
marker retention and no-live drain for table-bearing drop, marker-clear live
recovery for empty existing drop, forced `.shm` rebuild, and ordinary native
exclusive reopen.

## Native Storage Impact

The preserved tables are InnoDB. MyLite does not reinterpret native metadata;
it coordinates the ownerless dictionary boundary and verifies durable reopen
behavior for the preserved native schema and tables.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-drop-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-missing-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-existing-drop-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-empty-drop-crash`
- Run the ownerless dictionary primitive test.
- Run normal `schema-idempotent-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run representative production schema selectors, ownerless DDL stress,
  production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- Live-peer recovery completes while the native file-operation marker remains
  clear for duplicate create, missing drop, missing create, and empty drop.
- Existing drop live-peer recovery keeps the native file-operation marker set
  until the final no-live drain when native table files were removed.
- Duplicate idempotent create recovery keeps the original schema defaults,
  leaves the original table column collation unchanged, and keeps plain
  duplicate create returning errno 1007.
- Missing idempotent drop recovery keeps the real schema/table present and the
  missing schema absent.
- Missing idempotent create recovery exposes the created schema defaults and
  supports post-recovery table writes.
- Existing idempotent drop recovery keeps the removed schema/table absent.
- Empty idempotent drop recovery keeps the removed schema absent while the
  native file-operation marker remains clear.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-drop-crash`
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-missing-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-existing-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-empty-drop-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-idempotent-ddl`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-alter-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-lifecycle`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-default-ddl`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-idempotent-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-lifecycle`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-default-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2 && ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`

## Risks And Follow-Up

- This covers deterministic no-op schema DDL crash recovery, not every schema
  lifecycle spelling.
- Broader randomized DDL oracle execution remains planned.
