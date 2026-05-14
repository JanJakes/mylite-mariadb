# NULL Row Storage Coverage

## Goal

Verify and document NULL value persistence for keyless MyLite-routed tables.
The current row path stores MariaDB record images; this slice proves that
nullable column bits in those record images survive insert, full scan,
close/reopen, and callback conversion.

## Non-Goals

- Do not change the row-page file format.
- Do not implement indexes, nullable-key semantics, uniqueness, update/delete,
  autoincrement, BLOB/TEXT overflow, or copy `ALTER` rebuilds.
- Do not claim broad typed-value or binary-safe `libmylite` APIs; direct
  execution still exposes SQLite-style text callbacks.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/field.h:1430-1460` reads field NULL state from `null_ptr` and
  `null_bit`, including `is_null_in_record()`.
- `mariadb/sql/field.h:1477-1484` documents that nullable table fields point
  into the null bitmap in `table->record[0]`.
- `mariadb/sql/handler.h:2268` records `TABLE_SHARE::null_bits` as NULL bits at
  the start of a record.
- `mariadb/sql/unireg.h:74-76` treats `table->s->reclength` as the copy size
  for full MariaDB record images.
- `mariadb/storage/mylite/ha_mylite.cc` stores exactly `table->s->reclength`
  bytes from `write_row()` into MyLite row pages.

These sources make NULL preservation a property of copying the complete record
image, as long as MyLite returns the same bytes to MariaDB during table scans.

## Compatibility Impact

`NULL` columns move from planned to partial coverage for keyless non-BLOB
MyLite-routed tables. The claim is limited to insert and full-scan SELECT paths
that are already supported by the row storage foundation.

Nullable indexes, unique-key NULL behavior, generated columns, BLOB/TEXT NULL
payload ownership, update/delete, and transaction rollback remain planned.

## Design

No storage format change is required. Keep the existing record-image path:

- `write_row()` rejects keyed and autoincrement tables;
- supported keyless rows are appended as raw MariaDB record images;
- `rnd_next()` copies stored record images back into MariaDB row buffers;
- MariaDB's field layer interprets nullable columns from the restored null
  bitmap.

The slice adds tests that insert both NULL and non-NULL values into nullable
fixed and variable columns, read them back through `mylite_exec()`, close and
reopen the database, and read them again.

## File Lifecycle

NULL row coverage changes only the primary `.mylite` file by appending the same
row pages already used for keyless rows. No new sidecars, journals, WAL, or
temporary durable files are introduced.

## Embedded Lifecycle And API

No public API changes are required. `mylite_exec()` callbacks must continue to
deliver SQL NULL values as `NULL` value pointers and non-NULL values as text.

Prepared statements and binary-safe typed values remain future public API work.

## Build, Size, And Dependencies

No new dependency is introduced. The default embedded baseline is unchanged.
The storage-smoke static handler build should not materially change because the
slice adds tests and docs only.

## Test Plan

1. Extend storage-engine smoke coverage with a keyless MyLite-routed table that
   has nullable fixed and variable columns.
2. Insert rows containing SQL NULL and non-NULL values.
3. Assert direct execution callbacks receive NULL pointers for SQL NULL values
   and text for non-NULL values.
4. Reopen the `.mylite` file and repeat the SELECT assertions.
5. Run `dev`, `storage-smoke-dev`, `embedded-dev`, format checks, clang-tidy,
   and `git diff --check`.

## Acceptance Criteria

- Keyless routed tables persist nullable fixed and variable fields.
- SQL NULL values round-trip as NULL callback values before and after reopen.
- Non-NULL values in the same nullable columns remain readable.
- Compatibility docs mark NULL columns as partial, scoped to the covered
  keyless non-BLOB row path.

## Implementation Status

Implemented as storage-engine smoke coverage and documentation:

- A keyless MyLite-routed table with nullable `VARCHAR` and `INT` columns is
  inserted through direct SQL execution.
- SQL NULL values are verified as NULL callback pointers, and non-NULL values
  in the same nullable columns are verified as text.
- The same assertions run after closing and reopening the primary `.mylite`
  file.
- Compatibility and architecture docs now mark NULL columns as partial support
  for the keyless non-BLOB row path.

## Risks And Open Questions

- BLOB/TEXT NULL handling still needs the overflow-storage slice because the
  current raw record image may contain pointer-owned payload state for long
  values.
- Nullable-key semantics cannot be claimed until indexes exist.
- This slice does not add crash recovery, rollback, or update/delete behavior.
