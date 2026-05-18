# MTR Charset Weight And LIKE Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.ctype_latin2`, `main.ctype_utf8mb3_bin`, `main.ctype_utf8mb4_bin`, and
`main.ctype_utf8mb4_general_ci`. This adds curated embedded baseline coverage
for Latin2, UTF-8 binary and general collations, `WEIGHT_STRING()`, datetime
coercion under non-default connection character sets, and LIKE condition
propagation.

## Non-Goals

- Broad charset, UCA, Unicode, locale, or collation MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Enabling tests that require native MyISAM, Sequence, debug-only execution,
  Oracle SQL mode, host-file SQL I/O, XML functions, or disabled CSV log
  tables.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_latin2.test` sets Latin2 connection
  character sets and sources `include/weight_string.inc`,
  `include/weight_string_l1.inc`, and `include/ctype_datetime.inc`.
- `mariadb/mysql-test/main/ctype_utf8mb3_bin.test`,
  `mariadb/mysql-test/main/ctype_utf8mb4_bin.test`, and
  `mariadb/mysql-test/main/ctype_utf8mb4_general_ci.test` exercise UTF-8
  binary/general collation behavior through shared charset include files,
  including LIKE condition propagation.
- `mariadb/mysql-test/include/weight_string.inc`,
  `mariadb/mysql-test/include/ctype_special_chars.inc`,
  `mariadb/mysql-test/include/ctype_datetime.inc`,
  `mariadb/mysql-test/include/ctype_like_cond_propagation.inc`, and
  `mariadb/mysql-test/include/ctype_like_cond_propagation_utf8_german.inc`
  emit `SHOW CREATE TABLE` for CTAS tables that use the MTR profile's default
  engine.
- Under the MyLite MTR smoke profile, those `SHOW CREATE TABLE` outputs differ
  only in the known profile-specific default engine text:
  `ENGINE=Aria ... PAGE_CHECKSUM=1` instead of upstream MyISAM output.
- Probed nearby candidates remain outside this slice:
  - `main.ctype_cp1250_ch` reaches an explicit disabled native MyISAM section.
  - `main.ctype_dec8` reaches disabled Oracle SQL mode.
  - `main.ctype_like_range` is debug-build-only.
  - `main.ctype_mb`, `main.ctype_recoding`, and broader charset suites need
    separate review because their shared includes and output drift are broader.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected Latin2 and UTF-8 binary/general collation behavior in addition to
existing ASCII, legacy, UTF-32, charset diagnostics, collation-default, and
column/context collation coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

- Add the selected charset tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` statements in the shared upstream include files.
- Keep the normalization narrow: map `ENGINE=Aria` to `ENGINE=MyISAM` and
  remove ` PAGE_CHECKSUM=1`.
- Keep tests with explicit disabled engines, disabled server surfaces, or
  larger unrelated result drift outside the curated list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.ctype_latin2 main.ctype_utf8mb3_bin main.ctype_utf8mb4_bin main.ctype_utf8mb4_general_ci`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected charset tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Upstream test-source normalization is limited to profile-specific default
  engine text around shared `SHOW CREATE TABLE` outputs.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The normalization lives in shared MTR include files, so future broader MTR
  admission should continue checking that the replacement only hides the
  configured default-engine drift.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing charset behavior.
