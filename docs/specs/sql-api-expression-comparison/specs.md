# SQL API Expression Comparison

## Problem

The SQL API comparison harness already checks one representative direct result
set, prepared values and metadata, statement effects, and warnings against a
raw MariaDB embedded baseline. That proves the comparison path works, but it
does not exercise enough direct expression behavior to catch regressions in
common table-free SQL surfaces used by application probes, migrations, and
feature detection.

This slice expands the existing comparison binary with a deterministic direct
expression matrix while keeping full MTR-scale comparison out of scope.

## Scope

- Compare additional table-free `mylite_exec()` result sets against
  `mysql_query()` / `mysql_store_result()` from the MariaDB embedded baseline.
- Cover representative conditional, NULL, numeric, predicate, date/time,
  string, cast, UNION, and NULL-safe comparison behavior.
- Compare column names, row values, row counts, NULL state, and warning counts
  for each selected expression query.
- Reuse the existing `compat-sql-comparison` harness group.

## Non-Goals

- Do not import, normalize, or expand MariaDB MTR in this slice.
- Do not compare durable MyLite storage behavior.
- Do not add application-schema coverage.
- Do not change the public C API.
- Do not make size-profile changes.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h:429-435` declares the connection-level result and
  diagnostic APIs used for comparison: `mysql_field_count()`,
  `mysql_affected_rows()`, `mysql_insert_id()`, `mysql_errno()`,
  `mysql_error()`, `mysql_sqlstate()`, and `mysql_warning_count()`.
- `mariadb/include/mysql.h:480-494` declares `mysql_query()` and
  `mysql_real_query()`, the direct SQL entry points that the raw baseline uses.
- `mariadb/include/mysql.h:412-418` declares `mysql_num_rows()`,
  `mysql_num_fields()`, `mysql_fetch_field_direct()`,
  `mysql_fetch_fields()`, `mysql_fetch_row()`, and
  `mysql_fetch_lengths()`, which are the result-set APIs used to capture raw
  MariaDB names, rows, NULL state, and byte lengths.
- `packages/libmylite/src/database.cc:760-768` exposes `mylite_exec()` through
  the public C API.
- `packages/libmylite/src/database.cc:1589-1626` executes direct SQL through
  the embedded MariaDB connection, stores result rows, captures statement
  effects, and captures warning rows for `mylite_exec()`.
- `mariadb/libmysqld/lib_sql.cc:1293-1313` copies embedded OK-packet affected
  rows and insert ids into the client-side connection structures without a
  network socket, which is why the raw embedded baseline is the right local
  comparison authority for public SQL API behavior.

## Design

Extend `mylite_embedded_comparison_test` with a `direct_expressions` observation
set. Both the raw MariaDB phase and the MyLite phase iterate over the same SQL
array, store each `QueryResult`, and compare the resulting vectors in order.

The selected queries are intentionally table-free:

- conditional/NULL behavior through `CASE`, `COALESCE`, and `NULLIF`;
- numeric arithmetic through `DIV`, `MOD`, and `ROUND`;
- `IN`, `NOT IN`, and NULL predicate behavior;
- deterministic date arithmetic and `TIMESTAMPDIFF`;
- string concatenation, replacement, and character length;
- scalar casts to integer and decimal values;
- a multi-row `UNION ALL` result set;
- `NOT` and NULL-safe equality.

The comparison also now checks `warning_count` inside `compare_query_results()`.
The matrix is selected to avoid warnings, so any new warning drift fails the
comparison without changing the existing explicit warning-row test.

## Compatibility Impact

This strengthens SQL API compatibility evidence for direct result-set
marshalling. It does not claim full SQL compatibility and does not replace the
long-term MTR-scale comparison goal.

## Single-File And Storage Impact

No storage behavior changes. The MyLite side opens a normal `.mylite` file, but
the added queries do not create durable tables or metadata. The raw MariaDB side
uses the existing temporary runtime directory.

## Embedded Lifecycle Impact

No lifecycle changes. The raw embedded runtime still starts, records its
observations, shuts down, and only then opens the MyLite runtime.

## Public API, File Format, And Dependency Impact

No public API, file-format, dependency, or production binary-size change.

## Test Plan

- Build the embedded comparison binary:
  `cmake --build --preset embedded-dev --target mylite_embedded_comparison_test`.
- Run the focused binary:
  `build/embedded-dev/packages/libmylite/mylite_embedded_comparison_test`.
- Run the SQL comparison harness group:
  `tools/mylite-compat-harness run sql-comparison`.
- Run the embedded preset:
  `ctest --preset embedded-dev`.
- Run static shell and diff checks:
  `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness` plus
  `tools/mylite-compat-harness tools/mylite-size-report`, and
  `git diff --check`.

## Acceptance Criteria

- The comparison binary compares the new direct expression matrix against raw
  MariaDB embedded behavior.
- Each expression comparison checks column names, row values, NULL state, row
  count, and warning count.
- The `sql-comparison` harness group runs the updated binary.
- Compatibility and roadmap docs describe the added coverage without claiming
  broad MTR-scale comparison.

## Risks And Open Questions

- The matrix remains representative, not exhaustive.
- Some upstream MTR tests that look attractive for expression coverage still
  depend on native MyISAM, disabled server functions, skipped embedded server
  paths, or expected-result files tied to a different default engine. Those
  should be handled by a separate MTR-normalization or MTR-profile slice rather
  than mixed into this API comparison.
