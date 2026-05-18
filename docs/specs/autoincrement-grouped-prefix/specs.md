# Autoincrement Grouped Prefix Allocation

## Goal

Support grouped `AUTO_INCREMENT` allocation when the autoincrement column is a
later part of a supported key, such as `PRIMARY KEY (category, id)`. The first
implementation should match the MyISAM/Aria handler model by deriving the next
generated value from existing rows in the same non-autoincrement key prefix.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` calls
  `get_auto_increment()` for later-in-key autoincrement columns on every row
  instead of reserving a statement-wide interval.
- `mariadb/storage/myisam/ha_myisam.cc:ha_myisam::get_auto_increment()` copies
  the key prefix before the autoincrement part, seeks the last row in that
  prefix, and returns that row's autoincrement value plus one.
- `mariadb/storage/maria/ha_maria.cc:ha_maria::get_auto_increment()` uses the
  same prefix lookup model for Aria.
- `mariadb/sql/sql_table.cc` requires a handler with `HA_AUTO_PART_KEY` before
  accepting an autoincrement column that appears later in a key; MyISAM and
  Aria advertise that capability.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()`
  currently always reads MyLite's table-local counter, which is correct for
  first-key autoincrement but not grouped prefixes.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_table_supports_auto_increment()`
  currently rejects definitions where the autoincrement column appears only
  after another key part.

MariaDB's `AUTO_INCREMENT` documentation describes first-key InnoDB compound
support separately from the MyISAM/Aria grouped-prefix form:

- <https://mariadb.com/docs/server/reference/data-types/auto_increment>

## Scope

- Accept routed durable tables whose autoincrement column appears later in a
  supported key and no first-key autoincrement key exists.
- Generate values per exact key prefix by finding current MyLite rows in that
  prefix and computing `max(auto_increment_column) + 1` for that prefix.
- Preserve the existing table-local counter for first-key single-column and
  first-key compound autoincrement tables.
- Cover explicit high values, repeated prefixes, new prefixes, close/reopen,
  supported keyed update/delete, and forced-index visibility.

## Non-Goals

- Durable per-prefix counter pages or B-tree-style prefix maximum lookup.
- Changing table-local autoincrement behavior for first-key definitions.
- Transaction-aware rollback of consumed generated values.
- Cross-process allocation guarantees beyond the current file lock behavior.
- Partition-aware grouped allocation.
- Broader `auto_increment_offset` / `auto_increment_increment` matrices, which
  are covered by follow-up offset/increment slices.

## Compatibility Impact

MyLite can move grouped per-prefix autoincrement allocation from planned to
partial coverage for the supported routed storage subset. This primarily maps
to MyISAM/Aria-style grouped sequence semantics. First-key routed InnoDB-style
tables continue to use table-local allocation.

## Design

Relax the autoincrement key gate so a table is writable when the
autoincrement field is either:

- the first part of at least one supported key; or
- a later part of at least one supported key.

Advertise `HA_AUTO_PART_KEY` from the MyLite handler so MariaDB's DDL layer
allows the later-in-key form to reach the handler, matching the MyISAM/Aria
source model. Because MyLite routes explicit `ENGINE=InnoDB` declarations to
the MyLite handler, those declarations also receive MyLite grouped-prefix
behavior rather than native InnoDB's first-key-only restriction.

For `get_auto_increment()`:

1. keep the existing table-local path when `next_number_keypart == 0`;
2. for grouped keys, serialize the current row's key prefix before the
   autoincrement part;
3. read current live index entries for the grouped key;
4. compare each entry's serialized prefix with the current prefix;
5. fetch only matching live rows and read the autoincrement field;
6. return the largest non-negative autoincrement value in the prefix plus one,
   or `1` for a new prefix;
7. reserve a single generated value so MariaDB calls back for the next row.

The first compatibility slice used a table-row scan. The current implementation
uses MyLite's live index-entry stream to narrow candidate rows. B-tree pages or
a direct prefix-maximum primitive remain future performance work.

## File Lifecycle

No file-format change is required. Grouped allocation derives its value from
live row and row-state data already stored in the primary `.mylite` file. Old
row versions remain ignored through the current live-row filtering path.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is exposed through
ordinary SQL inserts and MariaDB's handler autoincrement contract.

## Storage-Engine Routing

The MyLite handler owns the physical behavior for routed tables. The slice
documents grouped allocation as MyLite/MyISAM/Aria-style behavior. Native
InnoDB remains absent from the embedded profile; explicit `ENGINE=InnoDB`
table declarations are routed to MyLite and therefore use MyLite grouped
allocation when the later-in-key shape is declared.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced. The implementation
adds handler logic and tests only.

## Test Plan

- Add grouped-prefix tables using explicit MyISAM, Aria, and routed InnoDB
  requests.
- Insert repeated prefixes and verify each prefix starts at `1`.
- Insert explicit high values in one prefix and verify only that prefix
  advances.
- Verify a different prefix continues from its own maximum.
- Verify duplicate compound keys still fail.
- Verify supported update/delete and forced-index reads keep rows visible.
- Verify close/reopen preserves grouped allocation through live index entries
  and matching durable rows.
- Keep first-key compound autoincrement coverage intact.
- Run `git diff --check`, the focused storage-engine smoke binary,
  `ctest --preset storage-smoke-dev`, and `ctest --preset dev`.

## Acceptance Criteria

- Grouped later-in-key autoincrement DDL publishes MyLite catalog metadata for
  supported routed tables.
- Generated values are allocated per prefix and persist across close/reopen.
- Explicit values advance only their own prefix.
- First-key autoincrement behavior remains table-local.
- Docs and compatibility matrices distinguish grouped-prefix coverage from
  transaction-aware autoincrement rollback and future performance work.

## Risks And Open Questions

- The append-only index-entry implementation is O(index entries) per generated
  value. That is better than the earlier row scan, but large grouped-write
  workloads still need a real prefix-maximum lookup before performance is
  advertised.
- The current table-level `AUTO_INCREMENT` value may still reflect explicit
  high values for `SHOW CREATE TABLE`; grouped generation itself does not rely
  on that table-local counter.
