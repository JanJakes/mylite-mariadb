# MTR Parser And Expression Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.keywords`, `main.parser_stack`, `main.precedence`,
`main.statement-expr`, `main.cte_cycle`, `main.name_const_replacement`,
`main.implicit_char_to_num_conversion`, `main.item_types`, `main.round`, and
`main.sql_safe_updates`. This adds curated embedded baseline coverage for
identifier keyword handling, parser-stack growth, operator precedence,
expression use in non-SELECT statements, recursive CTE cycle handling,
`NAME_CONST()` planning, implicit character-to-number comparison, item clone
typing, numeric rounding, and safe-update key checks.

## Non-Goals

- Broad parser, optimizer, or expression MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing MTR cases whose only mismatch is default-engine output.
- Admitting tests that depend on disabled native engines, omitted system log
  tables, skipped embedded modes, or process-list/session metadata that the
  embedded profile does not expose.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/keywords.test` covers keyword and non-reserved-word
  use as table names, column names, labels, procedure variables, aliases, and
  dynamic statement text.
- `mariadb/mysql-test/main/parser_stack.test` drives deep expression nesting
  and nested stored-program blocks through SELECT, PREPARE/EXECUTE, view,
  procedure, function, and trigger paths.
- `mariadb/mysql-test/main/precedence.test` covers MariaDB operator precedence
  across logical, comparison, bitwise, arithmetic, interval, collation, and SQL
  mode combinations.
- `mariadb/mysql-test/main/statement-expr.test` covers subselect expression
  acceptance and rejection in non-SELECT statements, including
  `EXECUTE IMMEDIATE`, procedure calls, diagnostics statements, and `DO`.
- `mariadb/mysql-test/main/cte_cycle.test` covers recursive CTE `CYCLE`
  syntax, duplicate and missing cycle-column errors, view creation, BLOB/TEXT
  cycle keys, and bit-type cycle keys.
- `mariadb/mysql-test/main/name_const_replacement.test` covers MDEV-33971
  regressions where `NAME_CONST()` previously changed selected plans, updates,
  and collation-aware comparisons.
- `mariadb/mysql-test/main/implicit_char_to_num_conversion.test` covers
  implicit string and character literal comparisons against BIT, integer,
  floating, DECIMAL/NUMERIC, and YEAR columns through indexed lookups.
- `mariadb/mysql-test/main/item_types.test` covers item cloning and type
  compatibility regressions over derived tables, views, temporal values,
  bit-string literals, `ALL` subqueries, and static float functions.
- `mariadb/mysql-test/main/round.test` covers string-to-integer rounding and
  overflow behavior for signed and unsigned integer widths.
- `mariadb/mysql-test/main/sql_safe_updates.test` covers `sql_safe_updates`
  behavior for OR predicates that use primary or secondary keys and the
  expected rejection when index-merge access is disabled.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby parser/type candidates stay outside this slice:
  - `main.binary`, `main.temporal_literal`, `main.type_binary`,
    `main.type_int`, and `main.type_nchar` have default-engine result
    mismatches under the Aria-based embedded profile.
  - `main.empty_string_literal`, `main.identifier`, and `main.null` reach
    disabled native-engine or trimmed SQL-function surfaces.
  - `main.1st`, `main.limit`, `main.multi_statement`, and `main.overflow`
    depend on omitted system tables, status metadata, or session metadata
    details that are not stable coverage in the trimmed embedded profile.
  - `main.compound` is skipped for embedded server and therefore is not
    coverage.
  - `main.negation_elimination` has optimizer row-estimate differences under
    this profile and needs separate normalization review.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected parser, keyword, CTE-cycle, precedence, expression, type-coercion,
rounding, and safe-update behavior in addition to existing bootstrap,
CAST/CONVERT, CASE-family, numeric/type, date/temporal, comparison, predicate,
ORDER BY, UNION, prepared-statement, scalar-function, aggregate DISTINCT,
charset/collation, KDF, disabled-DES, and REGEXP coverage. This remains
curated MariaDB embedded baseline coverage, not broad MTR-scale comparison and
not MyLite storage-routing evidence.

## Design

- Add the selected parser and expression tests to `tools/mylite-mtr-harness`'s
  default curated list.
- Do not modify upstream test files for this slice.
- Keep skipped, disabled-engine, omitted-system-table, unstable-metadata, and
  result-normalization candidates outside the default list.

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
- `tools/mylite-mtr-harness run main.keywords main.parser_stack main.precedence main.statement-expr main.cte_cycle main.name_const_replacement main.implicit_char_to_num_conversion main.item_types main.round main.sql_safe_updates`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected parser and expression tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Some nearby tests only need expected-result normalization for the Aria-based
  smoke profile, but that should be handled as a separate normalization-policy
  slice rather than mixed into this pass-gated admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing parser or expression behavior.
