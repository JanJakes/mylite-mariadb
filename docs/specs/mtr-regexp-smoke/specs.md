# MTR REGEXP Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.func_regexp` and `main.func_regexp_pcre`. This adds curated embedded
baseline coverage for POSIX-style regexp operators and PCRE-backed regexp
functions without claiming broad MTR-scale compatibility.

## Non-Goals

- Broad charset, collation, or regexp MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Enabling disabled native engines or daemon-only server surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_regexp.test` covers table-backed `REGEXP` /
  `RLIKE` predicates, prepared regexp parameters, binary and NULL inputs,
  escaped NUL-byte matching, and different charset/collation paths through
  `include/ctype_regex.inc`.
- `mariadb/mysql-test/main/func_regexp_pcre.test` covers `REGEXP_REPLACE()`,
  `REGEXP_INSTR()`, and `REGEXP_SUBSTR()` over string, NULL, numeric, binary,
  charset, collation, and result-metadata paths.
- `include/ctype_regex.inc` and `main.func_regexp_pcre` otherwise pass with
  the same narrow `ENGINE=Aria` / `ENGINE=MyISAM` and `PAGE_CHECKSUM=1`
  `SHOW CREATE TABLE` normalization already used by existing curated MTR tests.
- Nearby candidates remain outside the curated list:
  - `main.func_like` explicitly requests disabled native MyISAM.
  - Broader charset REGEXP tests that source `include/ctype_regex.inc` should
    be admitted deliberately with their own charset/collation evidence rather
    than implicitly through this function-focused slice.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected REGEXP scalar-function behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/type/date/temporal-rounding,
parser/comment/comparison, operator, scalar-function, default-expression,
weight-string, string/format, aggregate DISTINCT, date-format, ASCII charset,
KDF, and disabled-DES coverage. This remains curated MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

- Add `main.func_regexp` and `main.func_regexp_pcre` to
  `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in `include/ctype_regex.inc` and
  `main.func_regexp_pcre`.
- Keep explicit disabled-engine LIKE coverage outside the list.

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
- `tools/mylite-mtr-harness run main.func_regexp main.func_regexp_pcre`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_regexp` and
  `main.func_regexp_pcre`.
- Both tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization around `SHOW CREATE TABLE`.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The shared `include/ctype_regex.inc` normalization can help later charset
  REGEXP tests, but those tests still need independent admission because their
  charset/collation surfaces are broader than this slice.
