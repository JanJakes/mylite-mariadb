# Ownerless DDL Stress Statement-Lock Retry

## Problem Statement

The ownerless DDL stress test intentionally starts multiple processes that run
dictionary-changing DDL while other processes update and read a stable InnoDB
table. A recent ownerless-stress run failed when two DDL workers received
`MYLITE_BUSY` with the message `ownerless dictionary statement lock is busy`.
The isolated rerun passed, which makes this a harness stability problem for
collecting concurrency evidence rather than a reproduced corruption case.

The stress workload should keep failing on unexpected SQL, native storage, or
metadata errors, but it should tolerate bounded MyLite statement-lock
contention that occurs before a statement enters MariaDB execution.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` `acquire_ownerless_statement_locks()`
  returns `MYLITE_BUSY` with MariaDB errno `0` when it cannot acquire the
  ownerless dictionary or table statement byte-range lock before SQL execution.
- `packages/libmylite/src/database.cc`
  `update_ownerless_statement_lock_timeout_after_successful_sql()` maps a
  successful session `SET lock_wait_timeout = N` to the MyLite statement-lock
  wait cache for that handle.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `run_ownerless_ddl_stress_worker()` previously used fatal `exec_ok()` for
  every DDL statement, so a transient pre-execution statement-lock miss aborted
  the whole stress case.
- The checksum, random transaction, and foreign-key graph stress helpers
  already use bounded retry loops for expected transient contention.

## Design

Add a DDL-stress-specific execution helper:

- retry only `MYLITE_BUSY` with MariaDB errno `0`, the MyLite statement-lock
  miss shape;
- keep native MariaDB `1205`/`1213` and all other errors fatal in this slice;
- bound retries by a wall-clock deadline, so a stuck statement lock still
  fails with worker, round, phase, SQL, and diagnostic error details;
- apply the helper to DDL stress DDL statements and the stable-table DML update
  statements that can wait behind a concurrent dictionary DDL statement lock;
- leave the generic `exec_ok()` behavior unchanged for the rest of the test
  suite.

The normal stress registration keeps the existing
`MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=8` shape. A new focused CTest registration
runs a two-round DDL stress with
`MYLITE_OWNERLESS_DDL_STRESS_LOCK_WAIT_TIMEOUT=0`; once all child workers have
opened ownerless handles and reached the start barrier, the parent briefly holds
the ownerless dictionary statement-lock byte in
`concurrency/mylite-statements.lock`, forcing immediate statement-lock misses
under contention and exercising the retry path without changing product
defaults.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or directory layout
changes. The change affects only test harness behavior for a workload that
deliberately creates ownerless DDL/DML contention.

The slice does not expand supported SQL table-lock behavior. SQL-level
`LOCK TABLES` and local table-wait callback reachability remain documented
separately as unsupported or unclaimed research.

## Database Directory And Lifecycle Impact

No database-directory layout changes. The retry loop only reattempts statements
that failed at the ownerless statement-lock gate before native SQL execution.
Successful stress completion still verifies ownerless and ordinary reopen, a
forced `.shm` rebuild, final stable-table totals, absence of leftover stress
tables, and checkpointed ownerless WAL.

## Native Storage Impact

No native storage format changes. Native DDL/DML execution remains the same
once the MyLite statement lock is acquired.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The only CMake change is a focused CTest registration for the existing
stress binary.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` under the ownerless stress
  preset.
- Run the focused short-timeout retry CTest selector.
- Run the normal ownerless DDL stress selector.
- Run the production embedded build for the touched test target.
- Run `tools/check-ci-production-builds`, format check, `git diff --check`,
  and cleanup checks for ownerless test processes and `/tmp` directories.

## Acceptance Criteria

- The DDL stress helper retries only pre-execution MyLite statement-lock
  `MYLITE_BUSY` results.
- Unexpected MyLite, MariaDB, native lock-timeout, deadlock, metadata, and
  storage errors still abort the stress test with diagnostics.
- The focused short-timeout selector passes and exercises the retry path under
  deliberate statement-lock contention.
- The normal DDL stress selector passes with the existing eight-round stress
  profile.

## Verification Results

Local verification used production-shaped ownerless stress and PHP embedded
builds:

- `cmake --preset ownerless-stress`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress-statement-lock-retry$'
  --output-on-failure`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --preset php-embedded-prod`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `git diff --check`

Cleanup scans found no remaining ownerless test processes and no
`/tmp/mylite-ownerless-*` directories.

## Risks And Unresolved Questions

- This is not a product-level wait-queue change; applications still receive
  `MYLITE_BUSY` according to their configured statement-lock timeout and must
  decide their own retry policy.
- Longer external MariaDB/RQG stress and the broader DDL/file-lifecycle
  recovery matrix remain separate ownerless completion work.
