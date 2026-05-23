# MTR Locale And Support Tool Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with accepted upstream tests
`main.locale`, `main.my_print_defaults`, `main.mysqltest_256`, and
`main.perror`. This adds curated embedded baseline coverage for locale
formatting behavior, upstream support-tool option parsing, mysqltest command
behavior, and perror output.

Also record newly probed non-coverage candidates in the MTR unsupported
inventory so future MTR expansion does not rediscover the same embedded-profile
skips, native-engine mismatches, disabled SQL surfaces, or profile result-text
differences.

## Non-Goals

- Broad locale, mysqltest, support-tool, or command-line MTR coverage.
- Running these tests against MyLite storage-engine routing.
- Re-enabling native MyISAM/InnoDB, Sequence, XML, GIS, Oracle SQL mode,
  metadata-lock plugins, status counters, log tables, host-file SQL I/O, or
  server-only profiles.
- Normalizing upstream expected-result files for profile-specific result drift.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/locale.test` covers locale-dependent date and time
  formatting without requiring external engines.
- `mariadb/mysql-test/main/my_print_defaults.test` covers the
  `my_print_defaults` support tool.
- `mariadb/mysql-test/main/mysqltest_256.test` covers mysqltest command
  handling.
- `mariadb/mysql-test/main/perror.test` covers the `perror` support tool.
- The selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed candidates intentionally left out of accepted coverage include tests
  skipped by upstream MTR for embedded, big-test, debug, platform, or
  ps-protocol profiles; tests that require disabled Sequence, native MyISAM,
  native InnoDB bootstrap, XML, GIS, Oracle SQL mode, metadata-lock plugin,
  log-table, status-metadata, or host-file SQL I/O surfaces; and tests whose
  expected result text is specific to the broader upstream server profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected locale and support-tool behavior. Known unsupported/probed candidates
remain non-coverage and do not change SQL, C API, storage-engine, or file-format
behavior.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Add the understood non-coverage probes to the harness unsupported inventory
  with compact reason categories.
- Do not modify upstream MariaDB test files.
- Keep skipped, disabled-engine, unsupported-surface, and profile-drift
  candidates outside the accepted list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice expands opt-in MariaDB embedded MTR
baseline coverage and probe inventory only.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe main.mysqltest_ps main.my_print_defaults main.perror main.mysqltest_256 main.locale`
- `tools/mylite-mtr-harness run main.locale main.my_print_defaults main.mysqltest_256 main.perror`
- `tools/mylite-mtr-harness coverage`
- `tools/mylite-mtr-harness list-unsupported`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected locale and support-tool
  tests.
- All selected tests pass under the MyLite MTR smoke profile.
- The unsupported inventory contains the newly understood non-coverage probes
  without overlapping accepted curated tests.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Verification Results

- `tools/mylite-mtr-harness probe main.mysqltest_ps main.my_print_defaults main.perror main.mysqltest_256 main.locale`: 4 passed, 0 failed, 1 skipped (`main.mysqltest_ps`, ps-protocol profile).
- `tools/mylite-mtr-harness run main.locale main.my_print_defaults main.mysqltest_256 main.perror`: all 4 selected tests passed.
- `tools/mylite-mtr-harness run`: 8 MyLite profile tests, 204 upstream `main`
  tests, and 194 upstream `sys_vars` tests passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 398 accepted upstream baseline tests, 8 accepted MyLite profile tests, 17 accepted MyLite storage-routed tests, 423 accepted total tests, and 87 known unsupported upstream probes.
- `tools/mylite-mtr-harness list-unsupported`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- Broader locale, support-tool, and mysqltest suites need separate
  disabled-surface and profile-output normalization review.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing query behavior.
