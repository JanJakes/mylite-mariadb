# Column Metadata API

## Goal

Expose richer prepared-statement result metadata through `libmylite`, including
native MariaDB field type, flags, charset, display length, decimals, schema,
table, original table, and original column names.

This fills the next SQL execution API gap after typed values and warnings.

## Non-Goals

- Do not expose raw `MYSQL_FIELD`, `MYSQL_RES`, or `MYSQL_STMT` handles.
- Do not add direct-execution result metadata beyond the existing callback
  column names.
- Do not add parameter metadata.
- Do not translate MariaDB charset numbers to collation names in this slice.
- Do not implement streaming values, multi-results, or comparison suites.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` defines `MYSQL_FIELD` with `name`, `org_name`,
  `table`, `org_table`, `db`, `catalog`, `length`, `max_length`, `flags`,
  `decimals`, `charsetnr`, and `type`, plus string length fields for metadata
  names.
- `mariadb/include/mysql.h` declares `mysql_stmt_result_metadata()` and
  `mysql_fetch_fields()`, which MyLite already uses while preparing statements.
- `mariadb/include/mysql_com.h` defines `enum_field_types`; callers that need
  MariaDB-native type identity should receive this numeric value without
  exposing the raw enum as a required public header dependency.
- `mariadb/include/mysql.h` defines field flags such as `UNSIGNED_FLAG`,
  `PRI_KEY_FLAG`, `NOT_NULL_FLAG`, and `BLOB_FLAG`. MyLite can expose the raw
  flag mask for compatibility adapters while keeping the primary value-type API
  stable.

## Design

Keep the existing simple value APIs and add metadata accessors:

- `mylite_column_database_name()`,
- `mylite_column_table_name()`,
- `mylite_column_origin_table_name()`,
- `mylite_column_origin_name()`,
- `mylite_column_mariadb_type()`,
- `mylite_column_flags()`,
- `mylite_column_charset()`,
- `mylite_column_decimals()`,
- `mylite_column_length()`,
- `mylite_column_max_length()`.

String pointers are statement-owned and valid until `mylite_finalize()` on that
statement. Invalid statement handles or column indexes return `NULL` for string
metadata and zero for numeric metadata, matching the existing column access
style.

MyLite stores copies of the relevant `MYSQL_FIELD` metadata during
`mylite_prepare()` because MariaDB frees the result metadata object after
prepare-time inspection.

## Compatibility Impact

This moves prepared-statement metadata from planned to partial coverage:

- aliases remain available through `mylite_column_name()`;
- native schema/table/origin names are available for result columns backed by
  real table fields;
- expressions and literal columns may return empty metadata strings, matching
  MariaDB's metadata shape.

The API intentionally exposes MariaDB-native type and flag numbers as
compatibility data, while the stable MyLite value classification remains
`mylite_column_type()`.

## Single-File And Storage Impact

No file-format change is required. Metadata is copied from MariaDB's prepared
statement result metadata and remains statement-local.

## Embedded Lifecycle And API

Metadata pointers are owned by the statement. They are independent of current
row state and can be read after prepare and before the first `mylite_step()`.

## Build, Size, And Dependencies

No new dependency is added. The implementation stores fields already available
from `mysql_stmt_result_metadata()`.

## Test Plan

1. Extend public API NULL-column tests for the new metadata accessors.
2. Extend embedded prepared-statement tests for:
   - alias column names;
   - table-backed schema/table/origin metadata;
   - native type, flags, charset, decimals, and length values;
   - invalid column metadata access.
3. Add a compatibility harness group for column metadata.
4. Run `dev`, `embedded-dev`, `storage-smoke-dev`, the column-metadata
   compatibility group, format, tidy, diff checks, and size report.

## Acceptance Criteria

- Public header exposes the new metadata accessors.
- Metadata is available immediately after `mylite_prepare()`.
- Table-backed prepared results expose schema/table/original-column metadata.
- Invalid metadata access follows existing column getter behavior.
- Docs, compatibility matrix, roadmap, and harness describe partial metadata
  coverage.

## Risks And Open Questions

- MariaDB may return empty strings for expression metadata. MyLite should not
  synthesize table or origin names.
- Charset numbers are sufficient for this slice; resolving names can be added
  when the API has a broader collation story.
