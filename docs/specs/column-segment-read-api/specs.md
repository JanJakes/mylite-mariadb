# Column Segment Read API

## Goal

Add a bounded streaming-style column read API for prepared-statement TEXT/BLOB
results so callers can copy large values in byte ranges without requiring
`mylite_column_text()` or `mylite_column_blob()` to be the only access path.

This is the first large-value API slice after binary-safe prepared statements
and column metadata.

## Non-Goals

- Do not add streaming parameter binding.
- Do not add asynchronous or callback-driven row streaming.
- Do not keep column values alive after the next `mylite_step()`,
  `mylite_reset()`, or `mylite_finalize()`.
- Do not change `mylite_column_text()` or `mylite_column_blob()` semantics:
  they should still return full statement-owned values when requested.
- Do not implement multi-result handling.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `mysql_stmt_fetch_column(MYSQL_STMT *,
  MYSQL_BIND *, unsigned int column, unsigned long offset)`.
- `mariadb/include/mysql.h` documents `MYSQL_BIND::length` as the result
  length field and `MYSQL_BIND::buffer_length` as the caller-provided buffer
  size.
- `mariadb/include/mysql.h` defines `MYSQL_DATA_TRUNCATED` for prepared
  statement fetch status, and current MyLite prepared execution already sees
  this status when a variable-length result exceeds the initial bound buffer.
- Current MyLite prepared execution uses an initial variable-column buffer and
  calls `mysql_stmt_fetch_column()` to materialize truncated TEXT/BLOB values.
  The same MariaDB primitive can read byte ranges for a current row.

## Design

Expose:

```c
int mylite_column_read(
    mylite_stmt *stmt,
    unsigned column,
    size_t offset,
    void *buffer,
    size_t buffer_len,
    size_t *out_read);
```

The API is binary and encoding agnostic. It uses zero-based byte offsets into
the current column value. It returns `MYLITE_OK` with `*out_read == 0` when the
offset is at or beyond the value length. `buffer == NULL` is valid only when
`buffer_len == 0`. `out_read` is required so callers can distinguish empty
chunks from misuse.

For TEXT/BLOB columns on the current row, MyLite reads the requested range
through `mysql_stmt_fetch_column()` unless the value has already been
materialized in statement-owned memory. `mylite_column_text()` and
`mylite_column_blob()` materialize the full value on demand so existing pointer
semantics stay intact.

For NULL values, `mylite_column_read()` returns `MYLITE_OK` and reads zero
bytes. For non-variable columns, missing current rows, invalid indexes, or bad
output pointers, it returns `MYLITE_MISUSE`.

## Compatibility Impact

This moves streaming large values from planned to partial coverage for
prepared result columns. It gives applications a bounded-copy path for large
TEXT/BLOB values while preserving existing full-value APIs.

Streaming parameter binding, server cursor behavior, and wire-protocol
streaming remain planned.

## Single-File And Storage Impact

No file-format change is required. The API reads the current MariaDB prepared
statement row; storage behavior remains unchanged.

## Embedded Lifecycle And API

The read is valid only while the current row is active. Segment reads after
`MYLITE_DONE`, before the first row, or after reset/finalize return
`MYLITE_MISUSE`.

The caller owns the destination buffer. MyLite does not allocate for
successful segment reads unless the caller uses full-value pointer APIs.

## Build, Size, And Dependencies

No new dependency is added. The implementation uses MariaDB prepared statement
entry points already linked by the prepared statement API.

## Test Plan

1. Extend public API NULL-column tests for `mylite_column_read()`.
2. Extend embedded prepared-statement tests for:
   - chunked reads from TEXT/BLOB values larger than the initial buffer;
   - offset-at-end behavior;
   - NULL value reads;
   - misuse on invalid indexes, missing current rows, and non-variable
     columns;
   - full `mylite_column_blob()` access still working after segment reads.
3. Add a compatibility harness group for large column reads.
4. Run `dev`, `embedded-dev`, `storage-smoke-dev`, the large-value
   compatibility group, format, tidy, diff checks, and size report.

## Acceptance Criteria

- Public header exposes `mylite_column_read()`.
- Segment reads return exact byte counts and preserve embedded NUL bytes.
- Full TEXT/BLOB pointer access still materializes complete values.
- Misuse cases return stable MyLite result codes.
- Docs, compatibility matrix, roadmap, and harness describe partial large-value
  streaming coverage.

## Risks And Open Questions

- MariaDB's prepared statement API owns the current row. Segment reads cannot
  survive stepping to the next row.
- `mylite_column_text()` and `mylite_column_blob()` still materialize full
  values; a future API can add caller-owned streaming for direct execution or
  parameter binding.
