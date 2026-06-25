# Ownerless Schema Lifecycle Live Recovery

## Problem Statement

Ownerless schema create, alter, and drop crash selectors already prove no-live
recovery after a writer dies at `dictionary-before-finish`. They still leave a
live peer unable to recover the dead dictionary generation, so another
ownerless opener observes `MYLITE_BUSY` until the final peer exits.

MyLite should recover representative successful `CREATE DATABASE`,
`ALTER DATABASE`, and table-bearing `DROP DATABASE` boundaries while another
ownerless peer remains live.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - parses `CREATE DATABASE opt_if_not_exists ident`,
    `ALTER DATABASE ident_or_empty`, and `DROP DATABASE opt_if_exists ident`.
- `mariadb/sql/sql_db.cc`
  - `mysql_create_db_internal()` locks the schema name, creates the native
    schema directory, and writes `db.opt`.
  - `mysql_alter_db_internal()` rewrites `db.opt`.
  - `mysql_rm_db_internal()` locks the schema, drops contained tables through
    `mysql_rm_table_no_locks()`, drops database objects, removes `db.opt`, and
    removes the schema directory.
- `packages/libmylite/src/database.cc`
  - ownerless dictionary DDL already exposes `dictionary-before-finish` after
    native SQL success and before ownerless dictionary finish publication.
  - live dead-owner cleanup can consume recoverable dictionary kinds through a
    native file-operation marker lane or a metadata-only lane.

## Design

Add distinct recovery kinds:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_SCHEMA`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_SCHEMA`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_SCHEMA`

Classify bounded named `CREATE DATABASE <identifier>`,
`ALTER DATABASE <identifier>`, and `DROP DATABASE <identifier>` statements into
those kinds after native SQL success. `CREATE` and `ALTER` accept the default
charset/collation option grammar exercised by the focused selectors; other
schema clauses stay outside this slice.

Register recovery lanes:

- `CREATE DATABASE` and `ALTER DATABASE` use metadata-only live recovery because
  their native durable boundary is the schema directory and/or `db.opt`, not
  an InnoDB file-operation marker.
- `DROP DATABASE` participates in both lanes. A table-bearing drop keeps the
  native file-operation marker durable while a peer remains live, while empty
  future drops can still recover through the metadata-only lane.

Promote the existing schema create, alter, and drop unsafe-hook selectors from
no-live-only recovery to held-live-peer recovery. Keep idempotent duplicate
create and missing drop schema no-ops out of this slice.

## Scope

In scope:

- Successful named `CREATE DATABASE` live recovery.
- Successful named `ALTER DATABASE` live recovery.
- Successful named table-bearing `DROP DATABASE` live recovery.
- Schema directory and `db.opt` presence/removal.
- Schema defaults through `INFORMATION_SCHEMA.SCHEMATA`.
- Native table presence/absence for the existing schema crash selectors.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `SCHEMA` synonym spellings.
- `CREATE DATABASE IF NOT EXISTS` duplicate no-op recovery.
- `DROP DATABASE IF EXISTS` or `DROP SCHEMA IF EXISTS` missing no-op recovery.
- `CREATE OR REPLACE DATABASE`.
- `ALTER DATABASE COMMENT` and upgrade forms.
- Crash injection inside native directory creation, `db.opt` writes, or
  MariaDB's internal schema-drop table loop.
- SQL-level table-lock fault injection.
- External MariaDB/RQG stress.

## Compatibility Impact

No successful SQL behavior changes. The slice strengthens ownerless recovery
for MariaDB-compatible schema lifecycle statements when a writer dies after
native completion and before ownerless dictionary finish.

## Directory And Lifecycle Impact

No directory layout changes. `CREATE DATABASE` and `ALTER DATABASE` recovery
must keep the native file-operation marker clear. Table-bearing
`DROP DATABASE` recovery must keep that marker durable while a peer remains
live and drain it only after final no-live recovery.

## Native Storage Impact

No storage format changes. MariaDB's native schema directory, `db.opt`, and
InnoDB file-per-table removals remain authoritative.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-schema-create-crash`
  - `dictionary-schema-alter-crash`
  - `dictionary-schema-drop-crash`
- Run adjacent schema selectors:
  - `dictionary-schema-idempotent-create-crash`
  - `dictionary-schema-idempotent-drop-crash`
  - `schema-lifecycle`
  - `schema-default-ddl`
- Run representative production selectors:
  - `schema-lifecycle`
  - `schema-default-ddl`
  - `ddl-broader`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- Schema create/alter live recovery completes while a peer remains live and the
  native file-operation marker remains clear.
- Schema drop live recovery completes while a peer remains live and the native
  file-operation marker remains set until the final peer exits.
- Recovered schema defaults, native directory/`db.opt` state, table state,
  ownerless/native reopen, and forced `.shm` rebuild match the existing schema
  crash selector expectations.
- The new recovery kinds are valid in the ownerless dictionary primitive.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-alter-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-drop-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-idempotent-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-lifecycle`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-default-ddl`
- `ctest --preset ownerless-test-hooks -R '^(libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-schema-create-crash|libmylite\.ownerless-dictionary-schema-alter-crash|libmylite\.ownerless-dictionary-schema-drop-crash|libmylite\.ownerless-dictionary-schema-idempotent-create-crash|libmylite\.ownerless-dictionary-schema-idempotent-drop-crash)$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-lifecycle`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test schema-default-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- Idempotent schema no-op live recovery is covered by
  `ownerless-schema-idempotent-ddl-crash`.
- `SCHEMA` synonym spellings, empty-schema drops, schema-drop intra-loop crash
  points, and external randomized DDL oracle execution remain planned.
