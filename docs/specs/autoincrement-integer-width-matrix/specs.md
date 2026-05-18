# Autoincrement Integer Width Matrix

## Goal

Broaden MyLite autoincrement overflow coverage across supported integer widths
for first-key and grouped-prefix allocation. Existing tests cover TINYINT,
unsigned SMALLINT offset rounding, and unsigned BIGINT maximum handling; this
slice adds direct boundary checks for signed SMALLINT, signed and unsigned
MEDIUMINT, signed and unsigned INT, and signed BIGINT.

## Non-Goals

- Do not claim exhaustive coverage for every signedness, offset, increment,
  and engine combination.
- Do not change transaction-aware autoincrement rollback behavior.
- Do not change storage-level grouped allocation from append-only index-entry
  scans to B-tree prefix navigation.
- Do not change the MyLite public API or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` asks the handler
  for generated values, stores accepted generated values in the autoincrement
  field, and reports overflow through the normal MariaDB error path.
- `mariadb/sql/handler.cc:compute_next_insert_id()` computes the next
  generated value while preserving overflow sentinels.
- `mariadb/sql/field.h` and `mariadb/sql/field.cc` define the maximum accepted
  integer values for signed and unsigned autoincrement fields.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` returns
  MyLite table-local or grouped-prefix candidates to MariaDB.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_read_grouped_auto_increment()`
  computes grouped-prefix candidates from live MyLite index entries.

## Compatibility Impact

Autoincrement support remains partial, but integer-width overflow coverage now
extends beyond the previous tiny/small smoke cases to representative signed and
unsigned widths through signed BIGINT. Exhaustive signedness plus
offset/increment matrices remain planned.

## Design

Add one storage-engine smoke matrix that creates first-key and grouped-prefix
tables for these integer definitions:

- `SMALLINT`,
- `MEDIUMINT`,
- `MEDIUMINT UNSIGNED`,
- `INT`,
- `INT UNSIGNED`,
- `BIGINT`.

For each case, insert the penultimate value explicitly, generate the maximum
value, verify the generated maximum, reject the next generated insert, and
verify no overflow row was published. Grouped-prefix cases also verify another
prefix still starts at `1` after one prefix exhausts.

## File Lifecycle

No file-format or companion-file change is required. First-key autoincrement
state and grouped-prefix index entries remain in the primary `.mylite` file.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL execution and existing diagnostics.

## Storage-Engine Routing

The first-key cases use routed `ENGINE=InnoDB`; grouped-prefix cases use
explicit `ENGINE=MyISAM`, which routes to MyLite while preserving MyISAM-style
grouped autoincrement semantics.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for the integer-width matrix.
- Update compatibility, storage architecture, roadmap, and adjacent
  autoincrement specs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Covered first-key widths generate the maximum value after an explicit
  penultimate row and reject the next generated row.
- Covered grouped-prefix widths do the same for one prefix while another prefix
  can still allocate from `1`.
- Overflow attempts do not publish rows.
- Docs narrow the remaining autoincrement gap to exhaustive offset/increment
  and transaction-aware rollback coverage plus storage-level B-tree lookup.

## Risks And Unresolved Questions

- This is still a representative matrix. Exhaustive coverage should include all
  signedness and width combinations under non-default offset/increment settings.
- Signed BIGINT overflow is covered here; unsigned BIGINT maximum-state
  behavior remains covered by the separate
  `autoincrement-bigint-unsigned-maximum` slice.
