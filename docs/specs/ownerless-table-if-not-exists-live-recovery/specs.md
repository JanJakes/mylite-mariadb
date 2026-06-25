# Ownerless Table If Not Exists Live Recovery

## Problem Statement

Ownerless table-idempotent DDL crash coverage proves duplicate
`CREATE TABLE IF NOT EXISTS` no-op durability, but the existing selector waits
for no-live recovery because the broad `CREATE TABLE` recovery kind assumes a
native file operation may need checkpoint proof. A duplicate create that is
known before native execution to target an existing table is metadata-only: it
must not create, replace, or remove native files.

MyLite should recover this bounded duplicate-create dictionary boundary while
another ownerless peer remains live, with the native file-operation marker
clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `CREATE TABLE` with `opt_if_not_exists`.
- `mariadb/sql/sql_table.cc` routes an existing table with `IF NOT EXISTS` to
  the warning/no-op path, while plain duplicate create returns errno 1050.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` chooses the ownerless dictionary recovery
  kind before native SQL execution, which lets a conservative metadata lookup
  distinguish existing-table no-ops from absent-table creates.

## Design

Add a metadata-only recovery kind:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_IF_NOT_EXISTS`

Classify only bounded `CREATE TABLE IF NOT EXISTS <identifier>` statements
where pre-execution `information_schema.tables` lookup proves the target name
already exists. The classifier runs before the broad plain-`CREATE TABLE`
classifier, so proven duplicate no-ops use the metadata-only live recovery lane
and absent-table creates fall through to the existing file-operation recovery
lane.

Keep the classifier conservative:

- reject `CREATE TEMPORARY TABLE`,
- reject replacement forms,
- reject `CREATE TABLE ... LIKE`,
- reject CTAS / `SELECT` forms,
- reject unqualified table names when no current schema is known,
- reject MyLite-tracked temporary table names,
- return false if metadata lookup fails.

Promote `dictionary-table-idempotent-create-crash` from no-live-only recovery
to the held-live-peer recovery pattern. The selector verifies the native
file-operation checkpoint-needed marker stays clear while the live peer remains
open, then releases the peer and reruns ownerless/native reopen and forced
`.shm` rebuild checks.

## Scope

In scope:

- Duplicate `CREATE TABLE IF NOT EXISTS` no-op recovery for existing ordinary
  table names proven before native execution.
- Native `.frm` and `.ibd` preservation for the original InnoDB table.
- Live-peer metadata-only recovery with the native file-operation marker clear.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTest registration for visible timing and failure
  attribution.

Out of scope:

- Absent-table `CREATE TABLE IF NOT EXISTS`, which creates native files and
  remains on the file-operation recovery path.
- Temporary tables.
- Partitioned tables.
- `CREATE OR REPLACE TABLE`.
- `CREATE TABLE ... LIKE`.
- `CREATE TABLE ... AS SELECT`.
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless crash
recovery for MariaDB-compatible duplicate `CREATE TABLE IF NOT EXISTS` no-op
behavior. Plain duplicate create continues to return errno 1050, and the
attempted duplicate definition must not alter the original table.

## Directory And Lifecycle Impact

No directory layout changes. The duplicate no-op keeps the original native
InnoDB files and does not set the ownerless native file-operation
checkpoint-needed marker. Recovery finishes the dead ownerless dictionary
generation while a peer remains live.

## Native Storage Impact

No storage format changes. MariaDB remains authoritative for native table
metadata and files; MyLite only records the ownerless dictionary boundary as a
metadata-only recoverable no-op after pre-execution metadata proves the target
exists.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run primitive recovery-kind coverage.
- Run focused selector:
  - `dictionary-table-idempotent-create-crash`
- Run registered standalone CTest:
  - `libmylite.ownerless-dictionary-table-idempotent-create-crash`
- Run adjacent table idempotent selectors:
  - `dictionary-table-idempotent-drop-crash`
  - `dictionary-table-idempotent-existing-drop-crash`
- Run production representative selector:
  - `table-idempotent-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- A killed duplicate `CREATE TABLE IF NOT EXISTS` writer reaches
  `dictionary-before-finish` and does not hang.
- A live peer can recover the dead dictionary owner.
- The native file-operation marker remains clear before and after recovery.
- The original table definition is preserved and the attempted `note` column
  remains absent.
- Plain duplicate `CREATE TABLE` still returns errno 1050.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same rows and metadata.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-create-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-table-idempotent-create-crash$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-existing-drop-crash`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test table-idempotent-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for `LIKE`, CTAS,
  replacement, temporary, partitioned, or absent-table create paths.
- Broader DDL/file-lifecycle recovery and external MariaDB/RQG stress remain
  open completion work.
