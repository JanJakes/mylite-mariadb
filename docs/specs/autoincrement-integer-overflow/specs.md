# Autoincrement Integer Overflow

## Goal

Cover MyLite-routed `AUTO_INCREMENT` allocation at representative integer-width
boundaries, including overflow rejection after the last valid generated value
and offset/increment rounding near a smaller unsigned maximum.

## Non-Goals

- Do not claim exhaustive coverage for every integer type, signedness, offset,
  and increment combination.
- Do not cover `BIGINT UNSIGNED` maximum-value generation or explicit
  `18446744073709551615`; that is covered by the separate
  `autoincrement-bigint-unsigned-maximum` slice.
- Do not add transaction-aware rollback of consumed generated values.
- Do not change the MyLite file format or public API.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` stores the selected
  generated value into `table->next_number_field` before row write and returns
  `HA_ERR_AUTOINC_ERANGE` when the generated value exceeds the field's maximum.
- `mariadb/sql/handler.cc:compute_next_insert_id()` rounds generated values to
  the active `auto_increment_offset + N * auto_increment_increment` sequence
  and returns `ULONGLONG_MAX` as its overflow sentinel.
- `mariadb/sql/field.h:Field_tiny::get_max_int_value()` returns `255` for
  unsigned `TINYINT` and `127` for signed `TINYINT`.
- `mariadb/sql/field.h:Field_short::get_max_int_value()` returns `65535` for
  unsigned `SMALLINT`.
- `mariadb/sql/field.cc:Field_tiny::store()` and
  `Field_short::store()` clamp out-of-range values and report
  `ER_WARN_DATA_OUT_OF_RANGE`; the handler promotes generated-value overflow to
  an autoincrement error before row publication.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` feeds
  table-local and grouped-prefix candidates through MyLite's matching rounding
  helper before returning the first generated value to MariaDB.

## Compatibility Impact

This moves representative small integer-width and overflow-boundary coverage
from planned to partial for MyLite-routed first-key and grouped-prefix
autoincrement tables. It does not claim full integer-width compatibility.

## Design

Add storage-engine smoke coverage for:

- signed `TINYINT` first-key allocation at `127`, followed by generated
  overflow rejection;
- unsigned `TINYINT` first-key allocation at `255`, followed by generated
  overflow rejection;
- unsigned `TINYINT` grouped-prefix allocation at `255`, followed by generated
  overflow rejection for that prefix while another prefix can still allocate;
- unsigned `SMALLINT` first-key and grouped-prefix allocation near `65535`
  under `auto_increment_offset=5` and `auto_increment_increment=10`.

All overflow checks assert that the failing row is not visible after the
statement fails.

## File Lifecycle

No file-format or companion-file change is introduced. Successful rows,
index entries, and first-key autoincrement state continue to live in the
primary `.mylite` file. Failed generated-overflow statements must not publish
rows.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL inserts and MyLite's existing direct execution diagnostics.

## Storage-Engine Routing

The first-key tables use requested `ENGINE=InnoDB`, which routes to MyLite.
Grouped-prefix tables use requested `ENGINE=MyISAM`, matching MariaDB's
MyISAM/Aria grouped autoincrement source model while still routing storage to
MyLite.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend `mylite_embedded_storage_engine_test` with integer-boundary coverage.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Signed and unsigned `TINYINT` first-key tables accept the last valid
  generated value and reject the next generated overflow.
- A grouped unsigned `TINYINT` table rejects overflow for one prefix without
  blocking allocation for a different prefix.
- Unsigned `SMALLINT` first-key and grouped tables round near the maximum under
  non-default offset/increment settings, accept `65535`, and reject the next
  generated value.
- Docs narrow the remaining autoincrement gap to exhaustive matrices and
  transaction-aware rollback.

## Risks And Unresolved Questions

- `BIGINT UNSIGNED` maximum-value handling is covered by the separate
  `autoincrement-bigint-unsigned-maximum` slice.
- This smoke coverage is representative, not exhaustive.
