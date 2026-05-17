# MTR Charset Edge Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.ctype_cp850`, `main.ctype_cp866`, `main.ctype_hebrew`, and
`main.ctype_utf32_def`. This adds curated embedded baseline coverage for
selected legacy charset LIKE escape handling, Hebrew-to-Unicode conversion,
and UTF-32 fulltext boolean syntax handling.

## Non-Goals

- Broad charset, collation, UCA, Unicode, or locale MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing broad charset suites that depend on disabled native MyISAM,
  server-side file I/O, trimmed Oracle SQL mode, debug-only features, or large
  `SHOW CREATE TABLE` matrices.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_cp850.test` covers CP850 comparison with
  `CASE` expressions and exhaustive CP850 `LIKE ... ESCAPE` preparation and
  execution for byte values `0x00` through `0xFF`.
- `mariadb/mysql-test/main/ctype_cp866.test` covers CP866 `LIKE ... ESCAPE`
  handling for `0xFF`.
- `mariadb/mysql-test/main/ctype_hebrew.test` covers conversion of Hebrew LRM
  and RLM bytes to UTF-8 through `ALTER TABLE ... CONVERT TO CHARACTER SET`.
- `mariadb/mysql-test/main/ctype_utf32_def.test` covers UTF-32 availability and
  the `ft_boolean_syntax` regression path under a UTF-32-capable profile.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby charset suites stay outside this slice:
  - `main.ctype_latin1`, `main.ctype_latin1_de`, `main.ctype_latin2_ch`,
    `main.ctype_cp1250_ch`, `main.ctype_cp1251`, `main.ctype_big5`,
    `main.ctype_eucjpms`, `main.ctype_euckr`, `main.ctype_gb2312`,
    `main.ctype_gbk`, `main.ctype_sjis`, `main.ctype_ujis`,
    `main.ctype_cp932`, `main.ctype_swe7`, `main.ctype_tis620`, and
    `main.ctype_ucs2_def` require disabled native MyISAM sections.
  - `main.ctype_binary` reaches the unsupported embedded `BENCHMARK()`
    function path.
  - `main.ctype_dec8` reaches trimmed Oracle SQL mode.
  - `main.ctype_ucs` reaches unsupported `SELECT ... INTO OUTFILE`.
  - `main.ctype_like_range` is debug-build-only and is treated as no coverage
    by the harness.
  - `main.ctype_latin2`, `main.ctype_mb`, `main.ctype_recoding`, and
    `main.ctype_utf16_def` need separate output-normalization review.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected ASCII, legacy, and UTF-32 charset edge behavior in addition to
existing bootstrap, CAST/CONVERT, CASE-family, numeric/type/date/temporal,
parser/comment/comparison, `IN` predicate, operator, scalar-function,
default-expression, weight-string, string/format, aggregate DISTINCT,
date-format, collation diagnostics/defaults, KDF, disabled-DES, and REGEXP
coverage. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add the selected charset tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Do not modify upstream test files for this slice.
- Keep broader charset suites with disabled native engine, trimmed server
  surface, or large normalization requirements outside the list.

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
- `tools/mylite-mtr-harness run main.ctype_cp850 main.ctype_cp866 main.ctype_hebrew main.ctype_utf32_def`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected charset edge tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Broader charset suites need separate disabled-engine, unsupported-surface,
  and normalization policies before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing charset behavior.
