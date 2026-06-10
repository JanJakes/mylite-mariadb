# Ownerless Statement Lock Timeout

## Problem

Ownerless read/write handles use directory-owned statement locks to serialize
dictionary-changing statements, transaction-end writes, and conservative
autocommit write classes. Those locks used a fixed 60 second wait. During
performance debugging, a one-round checksum-stress run showed that a native
prepared-statement/update stall could leave another writer polling
`concurrency/mylite-statements.lock` for the full internal timeout, hiding the
real wait source and making CI timing noisy.

MariaDB already exposes `lock_wait_timeout` for session-scoped SQL lock waits.
Ownerless statement locks are a MyLite layer, but callers expect the same
session knob to make lock waits fail quickly in test and diagnostic runs.

## Design

Cache a per-handle ownerless statement-lock timeout in milliseconds. The
default remains the existing 60 second internal wait. After a successful direct
or prepared `SET [SESSION|LOCAL] lock_wait_timeout = N`, update that cache to
`N * 1000` milliseconds. `SET ... lock_wait_timeout = DEFAULT` resets the cache
to the default 60 second wait.

The parser intentionally covers simple integer assignments already accepted by
the embedded SQL engine and reuses the existing system-variable qualification
rules, including `SET lock_wait_timeout = N`,
`SET SESSION lock_wait_timeout = N`, and
`SET @@session.lock_wait_timeout = N`. More complex expressions keep the
current cached value after MariaDB accepts the statement.

`acquire_ownerless_statement_locks()` uses the cached timeout for both the
dictionary statement lock and per-table/global write statement locks. Native
InnoDB row, table, and metadata-lock waits keep their existing MariaDB timeout
paths.

## Compatibility Impact

No public API, SQL result, storage format, or directory layout changes. The
change only bounds MyLite's ownerless statement-lock polling for sessions that
explicitly set `lock_wait_timeout`; sessions that do not set it keep the
previous 60 second behavior.

## Performance Impact

The hot path reads one cached integer before polling a statement byte-range
lock. `SET lock_wait_timeout` parsing happens only after successful SET
statements and avoids an extra SQL query per statement.

This slice does not optimize the broader checksum-stress native
prepare/update stall. The captured evidence was:

- a prepared worker can block inside `mylite_prepare()` before DML begins;
- a direct writer can block inside native execution while holding the
  conservative per-table statement byte;
- another writer then polls `mylite-statements.lock` until its statement-lock
  timeout expires.

Those findings remain a separate native prepare/startup and statement-lock
granularity performance slice.

## Verification Plan

- Build the production embedded target that contains `database.cc`.
- Run focused ownerless primitive and prepared-statement coverage under the
  production embedded preset.
- Run the CI production-build guard.
- Run formatting and diff checks.

## Verification Results

Local verification on 2026-06-10 used `build/php-embedded-prod`.

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.(ownerless-primitives|embedded-prepared-statement)$'
  --output-on-failure` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset format` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.
