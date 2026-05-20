# Prepared SQL API

## Goal

Add the first reusable prepared-statement API to `libmylite`, including
parameter binding, typed column access, binary-safe values, warning access, and
statement lifetime enforcement.

## Non-Goals

- Do not add an external MariaDB C API adapter.
- Do not add typed date, time, decimal, JSON, geometry, or metadata APIs beyond
  the primary value classification.
- Do not broaden read-only or concurrent-writer support.
- Do not perform size profile hardening.

## Design

`mylite_prepare()` wraps MariaDB prepared statements and returns an owned
`mylite_stmt`. Parameter indexes are 1-based. Column indexes are 0-based.

Supported parameter bindings:

- `NULL`,
- signed and unsigned 64-bit integers,
- `double`,
- text with explicit length or `MYLITE_NUL_TERMINATED`,
- blob bytes with explicit length.

The first implementation copies text and blob bytes during binding. That keeps
`MYLITE_STATIC`, `MYLITE_TRANSIENT`, and custom destructor inputs safe without
depending on caller lifetimes. Custom destructors are called after the bytes are
copied because MyLite no longer needs the caller-owned buffer.

Column access is valid only while `mylite_step()` has returned `MYLITE_ROW`.
`mylite_column_type()` maps MariaDB field metadata to the first public value
classes: `NULL`, signed integer, unsigned integer, double, text, and blob.
Binary payloads are returned with `mylite_column_blob()` and
`mylite_column_bytes()`, so embedded NUL bytes do not truncate values.
Large result values are fetched by actual byte length instead of allocating the
declared maximum width of large MariaDB column types such as `LONGBLOB`.

`mylite_close()` now returns `MYLITE_BUSY` while statements are active. Callers
must finalize statements before closing the database handle.

Warnings are exposed through `mylite_warning_count()` and `mylite_warning()`.
The first implementation reads MariaDB warnings with `SHOW WARNINGS` on demand
and stores returned message text on the database handle.

## Test Plan

1. Add API misuse coverage for NULL statement and warning calls.
2. Add embedded prepared-statement coverage for:
   - reusable insert statements,
   - signed and unsigned integer parameters,
   - double, text, blob, and NULL parameters,
   - binary payloads containing embedded NUL bytes,
   - typed column classification and accessors,
   - large BLOB result values that exceed the initial result buffer,
   - reset, clear bindings, finalize, and close-with-active-statement behavior,
   - warning count and warning lookup.
3. Run the compatibility query label to include prepared SQL in the grouped
   harness.
4. Run the full `dev` and `embedded-dev` verification set.

## Acceptance Criteria

- Public header declares the prepared-statement, binding, column, and warning
  APIs documented for this slice.
- Embedded tests prove reusable prepared statements and binary-safe values.
- Active statements prevent database close until finalized.
- Compatibility docs mark prepared statements, binary-safe values, and warnings
  as partial rather than planned.
