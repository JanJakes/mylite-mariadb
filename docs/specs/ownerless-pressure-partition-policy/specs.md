# Ownerless Pressure Partition Policy

## Problem Statement

Ownerless active-reader pressure intentionally returns `MYLITE_BUSY` for
supported write statements when retained page-version WAL reaches the configured
soft limit. Unsupported ownerless SQL must keep returning its explicit policy
error under the same retained-WAL pressure so applications and compatibility
docs can distinguish resource throttling from unsupported file-lifecycle
surfaces.

Partitioned-table DDL is already rejected in ownerless read/write mode because
it creates additional native metadata and partition tablespace lifecycle states
that the current ownerless file-lifecycle protocol does not coordinate. The
active-reader pressure policy matrix did not yet prove that partition policy
wins over the pressure throttle.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `CREATE TABLE ... PARTITION BY ...` and
  `ALTER TABLE ... PARTITION ...` variants into normal DDL command paths that
  can create `.par` metadata and per-partition native storage files.
- `packages/libmylite/src/database.cc:reject_unsupported_sql_policy()` calls
  `is_unsupported_ownerless_partition_statement()` while
  `db.ownerless_rw_open` is active and before
  `enforce_ownerless_page_log_limit_policy()`.
- `packages/libmylite/src/database.cc:is_unsupported_ownerless_partition_statement()`
  rejects `CREATE` or `ALTER TABLE` statements after a `TABLE` token if later
  identifier tokens include `PARTITION`, `PARTITIONING`, `PARTITIONS`,
  `SUBPARTITION`, `SUBPARTITIONING`, or `SUBPARTITIONS`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c:
  test_ownerless_rejects_partition_ddl()` already verifies the direct
  ownerless partition policy over create, alter, exchange, convert, and remove
  partitioning forms with ownerless/native reopen.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c:
  test_ownerless_active_reader_pressure_limit_blocks_write_classes()` already
  builds retained-WAL pressure and verifies supported writes return
  `MYLITE_BUSY` while other unsupported surfaces keep explicit policy errors.

## Scope And Non-Goals

In scope:

- Extend the retained-WAL pressure selector with partitioned `CREATE TABLE` and
  `ALTER TABLE ... PARTITION BY` rejection checks.
- Verify those checks return the explicit ownerless partition policy diagnostic,
  not `MYLITE_BUSY`.
- Verify the rejected partition table and partition metadata remain absent
  through the existing post-pressure ownerless/native reopen and forced
  `.shm` rebuild checks.
- Update compatibility docs to include partitioned-table DDL in the
  policy-before-pressure evidence.

Out of scope:

- Supporting partitioned tables in ownerless read/write mode.
- Adding a partition file-lifecycle log, partition tablespace recovery, or
  external partition oracle.
- Changing the runtime policy order unless the regression exposes a bug.
- Extending the standalone partition-policy selector.

## Design

Reuse the existing `active-reader-pressure-write-policy` selector:

1. Build the existing retained-WAL pressure state with a live repeatable-read
   snapshot pin.
2. Reopen a writer with `ownerless_page_log_limit_bytes` equal to the retained
   WAL size.
3. Assert supported write classes still return `MYLITE_BUSY`.
4. Add partitioned `CREATE TABLE` and `ALTER TABLE ... PARTITION BY` statements
   to the unsupported-policy section and assert the ownerless partition policy
   diagnostic.
5. Assert `information_schema.tables` and `information_schema.partitions` do not
   contain rejected partition metadata before and after pressure release,
   ownerless reopen, native reopen, and forced `.shm` rebuild.

## Compatibility Impact

No SQL surface becomes supported. The compatibility claim becomes sharper:
ownerless pressure throttling applies to supported writes, while partitioned
table DDL remains an explicit unsupported ownerless file-lifecycle class even
when retained page-version WAL is at the configured limit.

## Directory And Lifecycle Impact

No directory format changes. The regression ensures partition DDL does not
enter native `.par` or partition tablespace creation paths under pressure.

## Native Storage Impact

No native storage behavior changes. Partitioned InnoDB table files remain out
of scope for ownerless read/write mode until partition file-lifecycle
coordination is designed and tested.

## Public API Impact

No public API changes.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-prod`.
- Run focused `active-reader-pressure-write-policy` in `embedded-prod`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the embedded-prod ownerless SQL shard containing
  `test_ownerless_active_reader_pressure_limit_blocks_write_classes`.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed.

```text
cmake --preset embedded-prod
cmake --preset ownerless-test-hooks

cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test
cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test

build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy
build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy

build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_active_reader_pressure_limit_blocks_write_classes
ownerless-sql case pass index=53 name=test_ownerless_active_reader_pressure_limit_blocks_write_classes seconds=9

build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_active_reader_pressure_limit_blocks_write_classes
ownerless-sql case pass index=55 name=test_ownerless_active_reader_pressure_limit_blocks_write_classes seconds=9

ctest --preset embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.11$' --output-on-failure
libmylite.ownerless-cross-process-sql.11 ... Passed

tools/require-cmake-release-build build/embedded-prod
cmake_release_build_ok=build/embedded-prod

tools/require-cmake-release-build build/ownerless-test-hooks
cmake_release_build_ok=build/ownerless-test-hooks

tools/check-ci-production-builds
ci_production_build_audit_ok=.github/workflows/ci.yml

cmake --build --preset format-check-prod
git diff --check
```

## Acceptance Criteria

- Under retained-WAL pressure, partitioned `CREATE TABLE` and
  `ALTER TABLE ... PARTITION BY` return `MYLITE_ERROR`, MariaDB errno `0`, and
  the ownerless partition policy diagnostic rather than `MYLITE_BUSY`.
- Rejected partition metadata remains absent from `information_schema.tables`
  and `information_schema.partitions` before pressure release.
- Final ownerless/native reopen before and after forced `.shm` rebuild still
  observes the same absence while all supported post-pressure write classes
  succeed.

## Risks And Follow-Up

- This is pressure-policy ordering evidence, not ownerless partition support.
- Full partition support still needs partition metadata routing, tablespace
  lifecycle recovery, crash injection, and external oracle stress.
