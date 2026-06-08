# WordPress MySQLi Status Write Fast Path

## Problem

The WordPress PHPUnit job now has separate CI steps for Docker image build,
WordPress source fetch, MyLite PHP-extension build, dependency installation,
database preparation, a mysqli performance probe, and the PHPUnit suite. Local
same-host profiling on 2026-06-08 did not reproduce a branch PHPUnit slowdown:

- branch focused `Tests_DB`: PHPUnit `00:15.130`, shell real `32.160s`;
- main `4760d512` focused `Tests_DB`: PHPUnit `00:28.109`, shell real
  `52.373s`.

The ordinary WordPress mysqli path is close to main for process startup,
connect/open, reads, and prepared writes. The branch is faster than main for
direct no-result autocommit inserts because of the existing no-result query
fast path. The remaining ordinary mysqli overhead is adapter-side Zend work,
not ownerless engine work.

One hot adapter cost is status-property maintenance. Successful queries
currently rewrite `errno`, `error`, `connect_errno`, `connect_error`,
`affected_rows`, and `insert_id` on every call even when the values are already
correct.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_clear_error()` rewrites four public error properties on
  every successful connection/query path.
- `php_mylite_mysqli_sync_status()` rewrites `affected_rows` and creates a
  fresh `insert_id` string for every sync.
- These properties are declared public for mysqli compatibility, so a pure
  internal cached-state skip would be unsafe: user code can mutate them between
  operations. The safe optimization must compare the actual public property
  value before deciding to skip a write.

## Design

Add small internal helpers that update a link property only when the public
property currently differs from the required value:

- long-valued properties use `zend_read_property()` plus direct long compare;
- string-valued properties compare the current string bytes before calling
  `zend_update_property_string()` or `zend_update_property_str()`;
- `insert_id` uses the same string comparison so the common unchanged `0` path
  avoids allocating and writing a fresh Zend string.

Keep error paths semantically unchanged. Successful paths still repair public
properties if user code changed them before the next query.

## Compatibility Impact

No PHP API, SQL behavior, public C API, storage, or ownerless behavior changes.
Public mysqli properties keep their current visible values after connect,
successful queries, and failures. The regression test mutates the public status
properties and verifies the next successful query restores them.

## Directory And Lifecycle Impact

No durable file or directory changes.

## Build And Performance Impact

The fast path replaces repeated Zend property writes with property reads and
conditional writes. It should help hot success paths where status values stay
stable, especially repeated reads and direct no-result writes. The branch/main
performance conclusion remains: ordinary WordPress PHPUnit is already close to
or faster than main on the measured host; ownerless autocommit remains a
separate engine-side performance gap.

## Test Plan

- Build `mylite_mysqli_php_extension`.
- Run the PHP mysqli API test.
- Run reduced WordPress `perf-probe` and CI-sized local `perf-probe` when the
  warmed Docker image/build tree are available.
- Run focused WordPress `Tests_DB` through the split `phpunit` phase.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`,
  `cmake --build --preset format-check`, and `git diff --check`.

## Acceptance Criteria

- Successful query paths skip redundant status writes when public properties
  already match.
- Mutated public status properties are restored by the next successful query.
- Existing mysqli API coverage passes.
- WordPress perf probe continues to report process/connect and SQL-loop timing.

## Verification Results

Local verification on 2026-06-08 used the warmed WordPress tree and Docker image
with `MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56`:

- `cmake --build --preset php-embedded-dev --target
  mylite_mysqli_php_extension`: passed.
- `ctest --preset php-embedded-dev -L php --output-on-failure`: passed, 3
  tests in 5.31s.
- `MYLITE_WORDPRESS_PHASE=build-php
  tools/wordpress-phpunit-mysqli-mylite`: passed with
  `mylite_mariadb_embedded_seconds=66`, `mylite_php_build_seconds=11`, and
  `wordpress_total_seconds=80`.
- `MYLITE_WORDPRESS_PHASE=perf-probe
  MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=5
  MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=5
  MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=3000
  MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=1000
  tools/wordpress-phpunit-mysqli-mylite`: passed with stock PHP startup
  56.185ms, PHP+MyLite startup 64.490ms, process+connect 547.464ms,
  process+connect delta 482.974ms, in-process connect 394.154ms, repeated
  `SELECT 1` 268.31 ops/s, transaction-scoped inserts 408.72 ops/s, point
  selects 236.88 ops/s, prepared autocommit inserts 380.85 ops/s, and direct
  autocommit inserts 737.91 ops/s.
- `MYLITE_WORDPRESS_PHASE=phpunit
  tools/wordpress-phpunit-mysqli-mylite --filter Tests_DB`: passed, 651 tests
  with 3 skips; PHPUnit reported 14.781s and the isolated PHPUnit phase
  reported `wordpress_total_seconds=27`.
- `bash -n tools/wordpress-phpunit-mysqli-mylite`: passed.
- `cmake --build --preset format-check`: passed.
- `git diff --check`: passed.
