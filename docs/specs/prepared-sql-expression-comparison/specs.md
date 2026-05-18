# Prepared SQL Expression Comparison

## Problem

The SQL API comparison harness now covers direct table-free expression result
sets against a raw MariaDB embedded baseline. The prepared side still only
compares simple scalar parameter casts. That leaves a gap for regressions where
bound parameters participate in expression evaluation before result values are
marshalled through `libmylite`.

This slice adds one deterministic prepared expression query that exercises
parameter binding, expression evaluation, metadata, and row-value retrieval
without touching durable storage.

## Scope

- Compare a parameterized prepared statement against the raw MariaDB embedded
  `MYSQL_STMT` baseline.
- Cover integer, NULL, string, and date-string input bindings.
- Cover representative prepared conditional, NULL, `IN`, date arithmetic, and
  string concatenation behavior.
- Compare column names, MariaDB native type numbers, row values, row count, and
  NULL state.
- Reuse the existing `compat-sql-comparison` harness group.

## Non-Goals

- Do not expand to MTR-scale prepared-statement coverage.
- Do not add durable storage, DDL, or application-schema behavior.
- Do not change public API semantics.
- Do not add rich prepared parameter metadata; MariaDB does not expose it for
  this base line.
- Do not make size-profile changes.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares the prepared-statement APIs used by the
  raw baseline and by MyLite's public statement API: `mysql_stmt_prepare()`,
  `mysql_stmt_param_count()`, `mysql_stmt_bind_param()`,
  `mysql_stmt_execute()`, `mysql_stmt_result_metadata()`,
  `mysql_stmt_bind_result()`, `mysql_stmt_fetch()`, and
  `mysql_stmt_close()`.
- `mariadb/include/mysql.h:412-418` declares result metadata APIs used to copy
  raw column names and types.
- `packages/libmylite/src/database.cc:770-801` exposes `mylite_prepare()` and
  `mylite_step()` through the public C API.
- `packages/libmylite/src/database.cc` owns the public binding functions and
  maps MyLite scalar bindings to MariaDB `MYSQL_BIND` values before execution.
- `packages/libmylite/src/database.cc:1192-1218` exposes current-row TEXT/BLOB
  pointers and byte lengths used by the MyLite prepared result path.

## Design

Extend `mylite_embedded_comparison_test` with a
`prepared_expressions` observation. The raw MariaDB phase prepares the shared
SQL text, binds ten parameters through `MYSQL_BIND`, executes once, captures
metadata and one row of string outputs, then closes the statement. The MyLite
phase prepares the same SQL through `mylite_prepare()`, binds equivalent public
parameters, steps once, and copies names, native type numbers, and textual row
values.

The query casts non-string expression outputs to `CHAR` so the comparison can
use one stable row-value shape while still exercising parameterized expression
evaluation:

- `CASE WHEN ? > ?` over integer parameters;
- `COALESCE(?, 'fallback')` over a bound NULL;
- `? IN (?, ?)` over string parameters;
- `DATE_ADD(CAST(? AS DATE), INTERVAL ? DAY)`;
- `CONCAT(?, ?)`.

## Compatibility Impact

Prepared SQL API compatibility evidence now covers both simple bound scalar
casts and bound parameters inside representative expressions. The claim remains
representative and does not imply broad prepared-statement or MTR-scale
coverage.

## Single-File And Storage Impact

No storage behavior changes. The added prepared statement is table-free and
does not create durable MyLite metadata.

## Embedded Lifecycle Impact

No lifecycle change. The raw MariaDB runtime and MyLite runtime remain
sequential phases in one comparison binary.

## Public API, File Format, And Dependency Impact

No public API, file-format, dependency, or production binary-size change.

## Test Plan

- Build the embedded comparison binary:
  `cmake --build --preset embedded-dev --target mylite_embedded_comparison_test`.
- Run the focused binary:
  `build/embedded-dev/packages/libmylite/mylite_embedded_comparison_test`.
- Run the SQL comparison harness group:
  `tools/mylite-compat-harness run sql-comparison`.
- Run embedded, storage-smoke, and dev presets.
- Run `git diff --check` and shell syntax checks for repository tools.

## Acceptance Criteria

- The raw MariaDB and MyLite prepared phases use the same SQL text and
  equivalent bound values.
- The comparison checks names, native types, NULL state, and row values.
- The compatibility harness reaches the new prepared comparison through
  `sql-comparison`.
- Docs describe prepared expression comparison without broadening the MTR claim.

## Risks And Open Questions

- This remains one representative prepared query. Broader prepared expression
  and metadata suites belong in later comparison or MTR work.
- The query intentionally casts expression outputs to `CHAR`; future coverage
  should also compare typed numeric and temporal retrieval when the matrix gets
  broader.
