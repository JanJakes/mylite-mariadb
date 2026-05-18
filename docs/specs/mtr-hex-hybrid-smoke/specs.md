# MTR Hex-Hybrid Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream test
`main.type_hex_hybrid`. This adds curated embedded baseline coverage for
hex-hybrid constants, optimizer equality handling across literal formats, and
large unsigned rounding behavior.

## Non-Goals

- Broad numeric, decimal, GIS, or data-type MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Defining a general default-engine output-normalization policy.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_hex_hybrid.test` covers MDEV-16426 optimizer
  handling for equal-looking constants with different literal formats and
  character-set lengths.
- The same test covers MDEV-23320, MDEV-23368, and MDEV-23366 rounding and
  CTAS metadata behavior for large hex-hybrid constants, including
  `FLOOR()`, `CEILING()`, `ROUND()`, and `TRUNCATE()`.
- The selected test passes under the MyLite MTR smoke profile after the same
  profile-specific `ENGINE=Aria` / `ENGINE=MyISAM` and `PAGE_CHECKSUM=1`
  `SHOW CREATE TABLE` normalization already used by existing curated MTR
  tests.
- Nearby candidates probed in the same pass remain outside this slice because
  they reach disabled native MyISAM sections, trimmed XML/GIS/server-function
  surfaces, SSL skips, old fixture dependencies, or broader result
  normalization needs.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected hex-hybrid literal, optimizer comparison, and large unsigned rounding
behavior in addition to existing curated numeric and type coverage. This
remains MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

- Add `main.type_hex_hybrid` to `tools/mylite-mtr-harness`'s default curated
  list near the existing numeric/type tests.
- Add profile-sensitive `--replace_result` directives around the affected
  `SHOW CREATE TABLE` outputs in the upstream test.
- Keep broader candidate tests with disabled engines, unsupported embedded
  server surfaces, skipped features, or wider result drift outside the curated
  list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The test runs
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.type_hex_hybrid`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.type_hex_hybrid`.
- The test passes under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This remains curated MariaDB embedded baseline coverage and does not prove
  MyLite storage-routing behavior for CTAS, large unsigned values, or
  expression metadata.
- Repeated default-engine output drift in larger type tests still needs a
  deliberate normalization policy before those tests should be accepted.
