# MTR DML RETURNING Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.replace`, `main.bulk_replace`, `main.create_replace_tmp`,
`main.key_primary`, `main.insert_returning_datatypes`,
`main.replace_returning`, `main.replace_returning_datatypes`, and
`main.replace_returning_err`. This adds curated embedded baseline coverage for
`REPLACE`, bulk replacement over unique keys, temporary table
`CREATE OR REPLACE`, primary-key conversion lookups, and representative
`INSERT ... RETURNING` / `REPLACE ... RETURNING` result and diagnostic paths.

## Non-Goals

- Broad DML MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Admitting skipped generic `RETURNING` suites that require the Sequence
  engine.
- Normalizing optimizer or default-engine result differences in nearby query
  suites.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/replace.test` covers basic `REPLACE` over primary
  keys, auto-increment range errors, defaults, HEAP conversion, and view
  `CHECK OPTION` rejection.
- `mariadb/mysql-test/main/bulk_replace.test` covers multi-row `REPLACE` over
  multiple unique keys and validates table contents with `CHECK TABLE`.
- `mariadb/mysql-test/main/create_replace_tmp.test` covers `CREATE OR REPLACE`
  from a temporary table source and verifies the temporary/base-table lifecycle.
- `mariadb/mysql-test/main/key_primary.test` covers primary-key lookups and
  LIKE predicates over character primary keys, including DESCRIBE/EXPLAIN
  metadata for matching and non-matching key values.
- `mariadb/mysql-test/main/insert_returning_datatypes.test` covers
  `INSERT ... RETURNING *` for representative integer, BIT, FLOAT, DOUBLE,
  ENUM, SET, VARCHAR, DATE, DATETIME, TIMESTAMP, YEAR, and BOOL values across
  VALUES, multi-row VALUES, SET, duplicate-key update, and INSERT SELECT forms.
- `mariadb/mysql-test/main/replace_returning.test` covers
  `REPLACE ... RETURNING` for VALUES, multi-row VALUES, SET, SELECT, prepared
  statements, expression projections, function calls, subqueries, `table.*`,
  and expected diagnostics.
- `mariadb/mysql-test/main/replace_returning_datatypes.test` covers
  `REPLACE ... RETURNING *` across the same representative scalar datatype set
  as the insert-returning datatype test.
- `mariadb/mysql-test/main/replace_returning_err.test` isolates expected
  `REPLACE ... RETURNING` diagnostics for missing columns, aggregate misuse,
  multi-row subqueries, multi-column operands, self-reference, and SELECT forms.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby DML/query candidates stay outside this slice:
  - `main.insert_returning`, `main.delete_returning`, `main.order_by`,
    `main.group_by`, and `main.union` are skipped because they require the
    Sequence engine.
  - `main.insert_select`, `main.select`, `main.select_jcl6`, and `main.having`
    reach disabled native MyISAM sections.
  - `main.insert_update`, `main.delete`, and `main.update` require disabled
    native InnoDB startup options.
  - `main.group_by_null` reaches the trimmed XML `EXTRACTVALUE()` surface.
  - `main.having_cond_pushdown` reaches trimmed host-file SQL function
    behavior through `LOAD_FILE()`.
  - `main.select_safe`, `main.delete_single_to_multi`, and
    `main.update_single_to_multi` have optimizer result differences under the
    Aria-based embedded profile and need separate normalization review.
  - `main.truncate` reaches SQL HANDLER behavior, which is deliberately outside
    the embedded profile.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected `REPLACE`, `RETURNING`, temporary create-or-replace, and primary-key
lookup behavior in addition to existing bootstrap, parser/expression,
subquery, ORDER BY, UNION, prepared-statement, scalar-function, aggregate
DISTINCT, charset/collation, KDF, disabled-DES, and REGEXP coverage. This
remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add the selected DML tests to `tools/mylite-mtr-harness`'s default curated
  list.
- Do not modify upstream test files for this slice.
- Keep skipped, disabled-engine, trimmed-function, SQL HANDLER, and optimizer
  normalization candidates outside the default list.

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
- `tools/mylite-mtr-harness run main.replace main.bulk_replace main.create_replace_tmp main.key_primary main.insert_returning_datatypes main.replace_returning main.replace_returning_datatypes main.replace_returning_err`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected DML and RETURNING tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Some nearby query and DML suites may be admit-able after expected-result
  normalization, but the policy for those profile-specific diffs should be
  designed separately.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing DML behavior.
