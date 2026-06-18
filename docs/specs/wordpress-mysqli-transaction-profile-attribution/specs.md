# WordPress Mysqli Transaction Profile Attribution

## Problem Statement

The native-control fast path reduced focused WordPress PHPUnit `Tests_DB`
runtime from the earlier query-verb sample, but the remaining transaction cost
is still too coarse to choose the next optimization safely. The existing
`query_verb_transaction_*` profile rows group transaction starts, transaction
ends, and savepoint operations together.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` classifies
  `BEGIN`, `START`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, and `RELEASE` under the
  coarse transaction query-verb bucket.
- WordPress' PHPUnit base test lifecycle issues transaction scaffolding around
  many database tests, so aggregate transaction time can dominate focused
  `Tests_DB` timing without proving which transaction statement class is the
  next bottleneck.
- `mariadb/libmysqld/libmysql.c` implements embedded `mysql_commit()`,
  `mysql_rollback()`, and `mysql_autocommit()` as `mysql_real_query()`
  wrappers. This means the previous native-control path avoids MyLite outer
  result/status work for exact statements but does not prove that a broader
  MariaDB parser bypass is available for transaction starts.

## Design

Keep the existing `query_verb_transaction_*` aggregate counters unchanged and
add opt-in mysqli profile sub-buckets:

- `query_transaction_start_*` for first keywords `BEGIN` and `START`;
- `query_transaction_end_*` for first keywords `COMMIT` and `ROLLBACK`;
- `query_transaction_savepoint_*` for first keywords `SAVEPOINT` and `RELEASE`.

The classifier mirrors the existing first-keyword profile behavior and runs
only when `MYLITE_MYSQLI_PROFILE=1` is enabled. It records elapsed time on the
same query completion path as the coarse query-verb profile, including failures.

The WordPress PHPUnit helper copies the new per-process rows into the timing
summary and into aggregate/child-aggregate profile totals so production CI can
compare transaction-start cost against transaction-end and savepoint cost.

## Compatibility Impact

This slice is diagnostics-only. It does not change SQL execution, public C API
behavior, storage-engine behavior, ownerless locking, or WordPress test
selection. The new rows are emitted only by the existing opt-in mysqli profile.

## Test And Verification Plan

- Extended the focused mysqli profile PHP test with `START TRANSACTION`,
  `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK`.
- Required nonzero transaction start, end, and savepoint profile rows in the
  focused CTest pass regex.
- Ran the PHP embedded production build and focused profile CTest.
- Ran shell syntax, production-build guard, format, tidy, and whitespace checks.
- Ran the focused production WordPress `Tests_DB` profile path and confirmed the
  new timing rows appear in real PHPUnit output and in timing-summary aggregate
  rows.

```text
cmake --build --preset php-embedded-prod --target mylite_mysqli_php_extension -j2
ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile(-context)?$' --output-on-failure
bash -n tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod
cmake --build --preset tidy-prod
git diff --check
MYLITE_WORDPRESS_PHASE=build-php MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 MYLITE_WORDPRESS_BUILD_TARGETS='mylite_mysqli_php_extension' tools/wordpress-phpunit-mysqli-mylite
MYLITE_WORDPRESS_PHASE=prepare-db MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1 tools/wordpress-phpunit-mysqli-mylite
MYLITE_WORDPRESS_PHASE=phpunit MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1 MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1 MYLITE_WORDPRESS_TIMING_LABEL=transaction-profile-attribution-tests-db tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'
```

The focused mysqli profile CTest emitted
`query_transaction_start_calls=1`, `query_transaction_end_calls=1`, and
`query_transaction_savepoint_calls=2`.

The focused production WordPress `Tests_DB` sample emitted:

```text
mylite_mysqli_profile_query_calls=4010
mylite_mysqli_profile_query_ms_total=5655.145
mylite_mysqli_profile_query_verb_transaction_calls=1310
mylite_mysqli_profile_query_verb_transaction_ms_total=1414.291
mylite_mysqli_profile_query_transaction_start_calls=651
mylite_mysqli_profile_query_transaction_start_ms_total=708.722
mylite_mysqli_profile_query_transaction_end_calls=659
mylite_mysqli_profile_query_transaction_end_ms_total=705.569
mylite_mysqli_profile_query_transaction_savepoint_calls=0
mylite_mysqli_profile_query_transaction_savepoint_ms_total=0.000
wordpress_phpunit_reported_seconds=8.195
```

## Risks And Follow-Up

- First-keyword transaction attribution is intentionally diagnostic and does
  not claim that every `START` or `RELEASE` form has identical SQL semantics.
- If CI shows transaction-start remains dominant, a future optimization must be
  based on a safe MariaDB integration point with focused correctness tests, not
  on direct assumptions about server-internal `THD` helpers in the standalone
  MyLite target.
