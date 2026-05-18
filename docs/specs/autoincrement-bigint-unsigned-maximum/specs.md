# Autoincrement BIGINT UNSIGNED Maximum

## Goal

Allow explicit `18446744073709551615` values in MyLite-routed
`BIGINT UNSIGNED AUTO_INCREMENT` columns and preserve MariaDB-compatible
generated-value failure after the maximum has been reached.

## Non-Goals

- Do not make `ULONGLONG_MAX` a generated autoincrement value; MariaDB treats it
  as a storage-engine failure sentinel on the generated-value path.
- Do not change transaction-aware autoincrement rollback behavior.
- Do not add exhaustive integer-width matrices.
- Do not change the MyLite public API or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` treats
  `ULONGLONG_MAX` returned from `handler::get_auto_increment()` as
  `HA_ERR_AUTOINC_READ_FAILED` before storing a generated value.
- `mariadb/sql/handler.cc:compute_next_insert_id()` returns `ULONGLONG_MAX` as
  its overflow sentinel.
- `mariadb/sql/field.h:Field_longlong::get_max_int_value()` allows
  `ULONGLONG_MAX` for unsigned `BIGINT`.
- `mariadb/mysql-test/suite/parts/t/mdev_24610.test` and matching result file
  show explicit `18446744073709551615` insert into
  `BIGINT UNSIGNED AUTO_INCREMENT` succeeds for a MEMORY table, while a later
  generated `NULL` insert fails with `ER_AUTOINC_READ_FAILED`.
- `mariadb/storage/mylite/ha_mylite.cc` currently rejects
  `ULONGLONG_MAX` while advancing first-key, grouped-prefix, and volatile
  autoincrement state, which prevents the explicit maximum row from being
  stored.

## Compatibility Impact

This moves explicit `BIGINT UNSIGNED` maximum handling from planned to partial
coverage for MyLite-routed first-key, grouped-prefix, and MEMORY/HEAP
autoincrement tables. Generated values after the maximum remain errors, matching
the MariaDB handler sentinel behavior.

## Design

Change MyLite autoincrement state advancement so explicit maximum values publish
the row and advance the next-value state to `ULONGLONG_MAX` instead of returning
`HA_ERR_AUTOINC_ERANGE`.

For later generated values:

1. first-key tables read `ULONGLONG_MAX` from table-local state;
2. grouped-prefix tables derive `ULONGLONG_MAX` from the maximum live row in
   the matching prefix;
3. MEMORY/HEAP tables read `ULONGLONG_MAX` from volatile state;
4. `ha_mylite::get_auto_increment()` returns `ULONGLONG_MAX`, and MariaDB's
   existing `handler::update_auto_increment()` maps that to
   `ER_AUTOINC_READ_FAILED`.

Lower explicit values remain insertable after the maximum when they do not
violate unique keys, but they do not reduce the exhausted next-value state.

## File Lifecycle

No file-format or companion-file change is required. The existing
autoincrement state page can store `ULONGLONG_MAX`; MEMORY/HEAP state remains
volatile.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL inserts and existing direct execution diagnostics.

## Storage-Engine Routing

The coverage applies to routed `ENGINE=InnoDB`, explicit grouped
`ENGINE=MyISAM`, and explicit `ENGINE=MEMORY` / `ENGINE=HEAP` table
declarations.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced. The handler change
requires rebuilding the storage-smoke MariaDB archive.

## Test Plan

- Add storage-engine smoke coverage for first-key `BIGINT UNSIGNED` explicit
  maximum insert, lower explicit insert after maximum, generated failure after
  maximum, and close/reopen persistence.
- Add grouped-prefix coverage where one prefix reaches the maximum and another
  prefix can still allocate.
- Add MEMORY/HEAP coverage where explicit maximum succeeds, generated overflow
  fails, and reopen clears volatile rows/state.
- Rebuild the storage-smoke MariaDB archive with
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Explicit `18446744073709551615` inserts succeed for covered first-key,
  grouped-prefix, and MEMORY table shapes.
- Later generated inserts fail with the MariaDB autoincrement read-failed
  diagnostic and do not publish rows.
- Lower explicit values can still insert after the maximum when unique keys
  permit them.
- First-key exhausted state persists across close/reopen.
- Docs remove `BIGINT UNSIGNED` maximum-state handling from the remaining
  autoincrement gap.

## Risks And Unresolved Questions

- The generated failure is `ER_AUTOINC_READ_FAILED`, not
  `ER_WARN_DATA_OUT_OF_RANGE`, because that is MariaDB's sentinel behavior for
  `ULONGLONG_MAX` on the engine-generated path.
- Exhaustive integer-width and transaction-aware rollback coverage remain
  separate work.
