# Autoincrement Offset And Increment Matrix

## Goal

Broaden MyLite-routed `AUTO_INCREMENT` coverage from two representative
session settings to a small matrix of offset/increment pairs for both first-key
and grouped later-in-key allocation.

## Non-Goals

- Do not test all legal `1..65535` offset and increment pairs.
- Do not cover every integer width, overflow boundary, or negative explicit
  value; representative small-width overflow coverage is handled by the
  `autoincrement-integer-overflow` slice.
- Do not add transaction-aware rollback of consumed generated values.
- Do not add a storage format or public API change.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_auto_increment_increment` and
  `Sys_auto_increment_offset` both allow session values from `1` through
  `65535`.
- `mariadb/sql/handler.cc:compute_next_insert_id()` computes the lowest value
  strictly greater than the current candidate that satisfies
  `auto_increment_offset + N * auto_increment_increment`.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` passes the active
  session offset and increment to `handler::get_auto_increment()` and advances
  multi-row statement values through the same rounding helper.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` feeds
  table-local and grouped-prefix candidates into MyLite's matching rounding
  helper before returning the first generated value.

## Compatibility Impact

This moves MyLite from representative offset/increment coverage to broader
matrix coverage for the routed storage subset. It still does not claim
exhaustive coverage across all legal values, integer widths, or overflow
boundaries.

## Design

Add storage-engine smoke coverage for four pairs:

- `(offset=1, increment=2)`;
- `(offset=2, increment=2)`;
- `(offset=5, increment=10)`, matching the source-code example shape;
- `(offset=7, increment=3)`, covering offset greater than increment.

For each pair, create one first-key table and one grouped-prefix table. Assert:

1. the first two generated rows use the expected sequence values;
2. an explicit higher value advances the next generated value to the next
   rounded sequence value;
3. a second grouped prefix starts its own sequence and advances independently.

## File Lifecycle

No file-format or companion-file change is introduced. The tests write normal
row, index-entry, and autoincrement state into the primary `.mylite` file.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL session variable assignments and inserts.

## Storage-Engine Routing

The first-key tables use requested `ENGINE=InnoDB`, which routes to MyLite. The
grouped-prefix tables use requested `ENGINE=MyISAM`, matching MariaDB's
MyISAM/Aria grouped autoincrement source model while still routing storage to
MyLite.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend `mylite_embedded_storage_engine_test` with the matrix coverage.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- First-key allocation honors all covered offset/increment pairs.
- Grouped allocation honors all covered pairs independently per prefix.
- Explicit higher values round to the next value in the active sequence.
- Offset-greater-than-increment behavior is covered.
- Docs distinguish this matrix from remaining overflow and integer-width
  coverage.

## Implementation Status

Implemented in storage-engine smoke coverage for first-key and grouped-prefix
tables under `(1,2)`, `(2,2)`, `(5,10)`, and `(7,3)`.

## Risks And Unresolved Questions

- Exhaustive legal-value coverage is impractical in the smoke suite; broader
  generated or MTR-scale comparison can still expand this area.
- Overflow boundaries and smaller integer widths are covered only
  representatively by the separate `autoincrement-integer-overflow` slice.
