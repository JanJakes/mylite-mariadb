# WordPress PHPUnit DB Prefix Filter

## Problem

The production WordPress PHPUnit CI job splits database-focused tests into a
dedicated `^Tests_DB` test-only step before running the remaining non-isolated
suite. The remaining-suite filter excluded `Tests_DB` only when the class name
ended immediately or continued with `::`. That still matched WordPress database
classes such as `Tests_DB_Charset`, `Tests_DB_dbDelta`, and
`Tests_DB_RealEscape`, so part of the database shard could be re-run inside the
long remaining-suite step.

The duplicated coverage made the already large remaining step slower and made
its timing less honest as a non-database suite measurement.

## Source Findings

- The database shard runs `tools/wordpress-phpunit-mysqli-mylite --filter
  '^Tests_DB'`, which intentionally includes the `Tests_DB` class family.
- The pinned WordPress checkout contains:
  - `Tests_DB` in `tests/phpunit/tests/db.php`;
  - `Tests_DB_Charset` in `tests/phpunit/tests/db/charset.php`;
  - `Tests_DB_dbDelta` in `tests/phpunit/tests/db/dbDelta.php`;
  - `Tests_DB_RealEscape` in `tests/phpunit/tests/db/realEscape.php`.
- A local PCRE check showed the old remaining-suite prefix rejected
  `Tests_DB::test_bail`, but still matched `Tests_DB_Charset::test_charset`
  and `Tests_DB_dbDelta::test_delta`.
- CI timing steps are already production-build guarded: the WordPress job uses
  `build/wordpress-php-embedded-prod` with `CMAKE_BUILD_TYPE=Release`, the
  MariaDB embedded archive uses `MinSizeRel`, and timing/test-only steps repeat
  those guards before running.

## Design

Keep the database shard unchanged. Change the remaining-suite filter from an
exact `Tests_DB` class exclusion to a prefix exclusion:

```text
^(?!Tests_DB)
```

The existing exact class exclusions for process-isolated class-level tests and
exact method exclusions for process-isolated method-level tests remain in the
same filter. Add a static workflow-audit marker requiring the remaining-suite
filter to start with `^(?!Tests_DB)` so the old exact-class partition does not
return unnoticed.

## Compatibility Impact

No SQL, PHP API, mysqli, public C API, storage-engine, or directory-lifecycle
behavior changes. This only removes duplicate PHPUnit execution from CI timing.

## Build And Performance Impact

No production binary impact. The long non-isolated WordPress PHPUnit step stops
re-running database-prefix tests that belong to the dedicated database shard.
The exact time saved depends on runner load and WordPress test ordering, but
the timing attribution becomes correct: database tests are measured in the
database step, not in both database and remaining steps.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Extract the workflow filter and verify representative PCRE samples:
  `Tests_DB::...`, `Tests_DB_Charset::...`, `Tests_DB_dbDelta::...`, and
  `Tests_DB_RealEscape::...` must be rejected, while
  `Tests_Actions::test_simple_action` must still match.
- Run the production CTest audit entry:
  `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-10 used the current production workflow text:

- `bash -n tools/check-ci-production-builds`: passed.
- `tools/check-ci-production-builds`: passed and reported
  `ci_production_build_audit_ok`.
- Extracting `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER` from
  `.github/workflows/ci.yml` and applying it with Python `re` rejected
  `Tests_DB::test_bail`, `Tests_DB_Charset::test_charset`,
  `Tests_DB_dbDelta::test_delta`, and
  `Tests_DB_RealEscape::test_real_escape`, while matching
  `Tests_Actions::test_simple_action`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed, 1/1 tests.
- `git diff --check`: passed.

## Acceptance Criteria

- The CI database shard still runs `^Tests_DB`.
- The non-isolated remaining shard rejects every `Tests_DB*` class name.
- The production-build audit fails if the remaining shard loses the
  `^(?!Tests_DB)` prefix exclusion.
- Production build guards remain in every timing-producing CI step.
