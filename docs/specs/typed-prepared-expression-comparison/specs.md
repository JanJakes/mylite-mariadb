# Typed Prepared Expression Comparison

## Problem

The SQL API comparison harness now covers direct expression result sets and a
prepared expression query that casts expression outputs to text. The prepared
typed-value coverage still only checks simple scalar parameter casts. That
leaves a narrower gap: regressions where bound parameters participate in
prepared expression evaluation and the resulting values are marshalled through
typed `libmylite` column APIs.

This slice adds one deterministic prepared expression query that compares typed
numeric retrieval and native temporal/text metadata against the raw MariaDB
embedded `MYSQL_STMT` baseline.

## Scope

- Compare one parameterized prepared expression statement against raw MariaDB
  embedded behavior.
- Cover signed integer, unsigned integer, double, NULL predicate, date, and
  string expression outputs.
- Bind signed integer, unsigned integer, double, NULL, date-string, and text
  parameters.
- Compare column names, MariaDB native type numbers, typed numeric values,
  temporal/text values, and row count.
- Reuse the existing `compat-sql-comparison` harness group.

## Non-Goals

- Do not expand to MTR-scale prepared-statement coverage.
- Do not add durable storage, DDL, or application-schema behavior.
- Do not change public API semantics or expose new MyLite value types.
- Do not add rich prepared parameter metadata; MariaDB does not expose it for
  this base line.
- Do not make size-profile changes.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h:412-418` declares result metadata APIs used to copy
  raw column names and native type numbers.
- `mariadb/include/mysql.h` declares the raw prepared-statement APIs used by
  this comparison: `mysql_stmt_prepare()`, `mysql_stmt_param_count()`,
  `mysql_stmt_bind_param()`, `mysql_stmt_execute()`,
  `mysql_stmt_result_metadata()`, `mysql_stmt_bind_result()`,
  `mysql_stmt_fetch()`, and `mysql_stmt_close()`.
- `packages/libmylite/src/database.cc:1120-1172` exposes current-row typed
  column APIs for NULL, signed integer, unsigned integer, double, and text.
- `packages/libmylite/src/database.cc:2651-2703` binds prepared statement
  result buffers according to MariaDB metadata.
- `packages/libmylite/src/database.cc:2862-2928` maps MyLite bound values and
  MariaDB result metadata into MyLite public value types.

## Design

Extend `mylite_embedded_comparison_test` with a
`typed_prepared_expressions` observation. The raw MariaDB phase prepares the
shared SQL text, binds eleven parameters through `MYSQL_BIND`, executes once,
captures metadata, and retrieves one row through typed result buffers for
numeric outputs plus string buffers for temporal/text outputs. The MyLite phase
prepares the same SQL through `mylite_prepare()`, binds equivalent public
parameters, steps once, and reads values with `mylite_column_int64()`,
`mylite_column_uint64()`, `mylite_column_double()`, and
`mylite_column_text()`.

The selected query covers:

- `CAST(? + ? AS SIGNED)` for signed integer expression retrieval;
- `CAST(? * ? AS UNSIGNED)` for unsigned integer expression retrieval;
- `CAST(? / ? AS DOUBLE)` for double expression retrieval;
- `? IS NULL` for a bound-NULL predicate result;
- `DATE_ADD(CAST(? AS DATE), INTERVAL ? DAY)` without a text cast so native
  temporal metadata is still compared while the public MyLite value remains
  text;
- `CONCAT(?, ?)` for a prepared string expression.

## Compatibility Impact

Prepared SQL API compatibility evidence now covers both text-marshalled
prepared expression outputs and typed prepared expression retrieval. The claim
remains representative and does not imply broad prepared-statement or MTR-scale
coverage.

## Single-File And Storage Impact

No storage behavior changes. The added statement is table-free and does not
create durable MyLite metadata.

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
- Run embedded and storage-smoke comparison coverage.
- Run `git diff --check` and shell syntax checks for repository tools.

## Acceptance Criteria

- The raw MariaDB and MyLite prepared phases use the same SQL text and
  equivalent bound values.
- The comparison checks names, native types, typed numeric values, temporal/text
  values, and row count.
- The compatibility harness reaches the new typed prepared comparison through
  `sql-comparison`.
- Docs describe typed prepared expression comparison without broadening the MTR
  claim.

## Risks And Open Questions

- This remains one representative prepared query. Broader prepared expression
  and metadata suites belong in later comparison or MTR work.
- Temporal values still use MyLite's text value surface because the public API
  does not expose dedicated date/time types.
