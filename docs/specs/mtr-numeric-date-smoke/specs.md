# MTR Numeric And Date Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.bigint` and `main.adddate_454`. This adds curated embedded baseline
coverage for large integer parsing, unsigned BIGINT arithmetic and storage,
integer result metadata, CTAS integer inference, and representative
`ADDDATE()` date arithmetic without claiming broad MTR-scale compatibility.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Adding MTR to the default compatibility harness group set.
- Admitting tests that are skipped under embedded MTR or depend on disabled
  native engines or host-file SQL I/O.
- Broad numeric, date, time, or temporal type coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/bigint.test` exercises BIGINT and unsigned BIGINT
  literal parsing, arithmetic, `CONV()`, table-backed min/max/grouping paths,
  autoincrement with a high initial value, CTAS integer type inference, and
  decimal-to-integer range behavior.
- `mariadb/mysql-test/main/bigint.test` has two `SHOW CREATE TABLE` assertions
  after CTAS statements. Under the MyLite MTR smoke profile, the default engine
  is Aria because native MyISAM is disabled, so those statements need the same
  narrow `ENGINE=Aria` / `ENGINE=MyISAM` and `PAGE_CHECKSUM=1` normalization
  already used by `main.cast` and `main.case`.
- `mariadb/mysql-test/main/adddate_454.test` covers representative `ADDDATE()`
  behavior over day, month, year, time, and mixed interval forms.
- Verified commands:
  - `tools/mylite-mtr-harness run main.bigint`
  - `tools/mylite-mtr-harness run main.adddate_454`
- Nearby candidates are not yet suitable for the curated list:
  `main.binary_to_hex` is skipped for embedded MTR, `main.func_time` requires
  native MyISAM, and `main.type_date` reaches `SELECT ... INTO OUTFILE`, which
  is intentionally unsupported in the MyLite embedded profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected numeric and date behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, scalar operator, and string/format coverage. This
remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add `main.bigint` and `main.adddate_454` to
  `tools/mylite-mtr-harness`'s default curated list.
- Add two profile-sensitive `--replace_result` directives to
  `mariadb/mysql-test/main/bigint.test` immediately before the CTAS
  `SHOW CREATE TABLE` checks.
- Leave `main.adddate_454.test` unchanged because it passes under the profile
  without normalization.

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
- `tools/mylite-mtr-harness run main.bigint`
- `tools/mylite-mtr-harness run main.adddate_454`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.bigint` and `main.adddate_454`.
- Both tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is the profile-specific default
  engine text normalization in `main.bigint`.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The curated list is still small and baseline-only. A later MTR-scale
  comparison design must decide how to run equivalent SQL through MyLite's
  embedded API and routed storage without daemon-only assumptions.
