# Autoincrement Offset And Increment Multi-Row Coverage

## Goal

Broaden MyLite-routed `AUTO_INCREMENT` offset/increment coverage beyond the
initial representative slice by checking multi-row allocation after explicit
high values. This covers both table-local first-key allocation and grouped
later-in-key allocation.

## Non-Goals

- Do not claim exhaustive `auto_increment_offset` /
  `auto_increment_increment` matrices; broader pair coverage is handled by the
  `autoincrement-offset-increment-matrix` slice.
- Do not cover negative explicit values. Offset-greater-than-increment behavior
  is covered by the `autoincrement-offset-increment-matrix` slice, and
  representative small-width overflow behavior is covered by the
  `autoincrement-integer-overflow` slice.
- Do not add transaction-aware rollback of consumed generated values.
- Do not replace the grouped allocation path with a B-tree-style prefix-maximum
  lookup.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:compute_next_insert_id()` rounds generated ids to
  the active session `auto_increment_offset + N * auto_increment_increment`
  sequence.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` passes offset and
  increment into the storage-engine `get_auto_increment()` hook for each
  generated row.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` uses
  table-local durable state for first-key allocation and live-index-entry
  prefix maxima for grouped prefix allocation.

## Compatibility Impact

This moves a little more offset/increment behavior into covered partial support:
multi-row generated values after explicit high values are covered for first-key
and grouped-prefix routed tables. Exhaustive matrices remain planned.

## Design

Add storage-engine smoke coverage with `auto_increment_offset=3` and
`auto_increment_increment=4`:

1. First-key table:
   - insert three generated rows in one statement and expect `3, 7, 11`;
   - insert explicit id `30`;
   - insert two generated rows in one statement and expect `31, 35`;
   - reopen and expect the next generated value to be `39`.
2. Grouped later-in-key table:
   - insert interleaved generated rows for two prefixes and expect independent
     sequences;
   - insert explicit `(prefix=1, id=30)`;
   - insert multiple generated rows across old and new prefixes and expect
     each prefix to round independently;
   - reopen and verify each prefix still computes the next value from live
     rows.

No production change is expected unless the tests expose incorrect allocation
or reservation behavior.

## File Lifecycle

No file-format or companion-file change is introduced. First-key allocation
uses durable table autoincrement state. Grouped allocation uses live index
entries and matching rows in the primary `.mylite` file.

## Embedded Lifecycle And API

No public C API change is required. The behavior is exposed through ordinary
SQL session-variable assignment and inserts.

## Build, Size, And Dependencies

No dependency or intended binary-size-profile change is introduced. This is a
test and documentation slice unless coverage exposes a bug.

## Test Plan

- Add a storage-engine smoke test for multi-row post-explicit first-key and
  grouped-prefix allocation under non-default offset/increment values.
- Run the focused storage-smoke test, storage-smoke CTest preset, default CTest
  preset, format check, and `git diff --check`.

## Acceptance Criteria

- First-key multi-row inserts allocate the expected sequence before and after an
  explicit high id.
- Grouped-prefix multi-row inserts allocate the expected per-prefix sequences
  before and after an explicit high id.
- Close/reopen preserves first-key state and grouped live-row-derived next
  values.
- Docs continue to mark exhaustive integer-width coverage, `BIGINT UNSIGNED`
  maximum-state handling, and storage-level B-tree navigation as planned.

## Implementation Status

Implemented in storage-engine smoke coverage:

- First-key multi-row inserts allocate `3, 7, 11` under
  `auto_increment_offset=3` and `auto_increment_increment=4`.
- First-key multi-row inserts after explicit id `30` allocate `31, 35`, and
  close/reopen preserves the next generated value `39`.
- Grouped-prefix multi-row inserts allocate independent per-prefix sequences
  before and after explicit `(prefix=1, id=30)`.
- Close/reopen preserves grouped allocation by reading live index entries and
  rounding each prefix independently.
