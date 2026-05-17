# MTR String And Format Function Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.func_concat` and `main.func_format`. This adds curated embedded baseline
coverage for `CONCAT()`, `CONCAT_WS()`, formatting helper functions,
subquery/string interactions, selected procedure-backed string expressions,
and aggregate use of formatting helpers.

## Non-Goals

- Broad string-function MTR coverage.
- Tests that are skipped under embedded MTR or depend on disabled SQL
  functions, disabled engines, or optimizer row-estimate stability.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_concat.test` exercises `CONCAT()` and
  `CONCAT_WS()` across scalar values, grouped rows, unions, subqueries,
  regular expressions, joins, stored procedures, derived tables, and selected
  encryption/base64/hex helper interactions.
- `mariadb/mysql-test/main/func_format.test` exercises native
  `format_pico_time()` and `format_bytes()` behavior, including null, numeric,
  text, aggregate, scientific-notation, and large-value inputs.
- Both tests pass under the MyLite MTR smoke profile without source changes.
- Nearby string/function candidates remain outside the curated list:
  `main.func_str` is skipped because the Sequence engine is disabled for the
  profile. The later
  [MTR scalar function smoke](../mtr-scalar-function-smoke/specs.md) admits
  `main.func_replace`, and
  [MTR DEFAULT and weight string smoke](../mtr-default-weight-string-smoke/specs.md)
  admits `main.func_default` and `main.func_weight_string` with narrow
  default-engine normalization.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected string and format function behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, and scalar operator coverage. This remains curated
MariaDB embedded baseline coverage, not broad MTR-scale comparison.

## Design

Add `main.func_concat` and `main.func_format` to
`tools/mylite-mtr-harness`'s default curated list. Convert the list to a
multi-line Bash array so the growing smoke set remains readable.

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

- `tools/mylite-mtr-harness run main.func_concat`
- `tools/mylite-mtr-harness run main.func_format`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_concat` and
  `main.func_format`.
- Both tests pass under the MyLite MTR smoke profile.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Future string-function MTR expansion will need a clear policy for tests that
  otherwise pass but require repeated default-engine result normalization.
