# Autoincrement Grouped Transaction Rollback

## Goal

Cover grouped later-in-key `AUTO_INCREMENT` allocation across durable
transaction rollback and savepoint rollback. MyLite-routed grouped tables must
remove rolled-back rows and derive the next generated value from the live
per-prefix maximum, rather than inheriting first-key table-local gap
preservation.

## Non-Goals

- Do not change first-key table-local autoincrement rollback behavior.
- Do not add durable B-tree pages or a storage-level prefix-maximum primitive.
- Do not add a public storage API or storage file-format change.
- Do not expand trigger, view, or exhaustive expression-error matrices.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` treats
  autoincrement columns that are not first in the key as singleton intervals,
  so the SQL layer calls the handler again for the next generated grouped
  value.
- `mariadb/sql/handler.cc:handler::get_auto_increment()` uses the current row
  key prefix and returns one value for the not-first-in-index case.
- `mariadb/storage/myisam/ha_myisam.cc:ha_myisam::get_auto_increment()` and
  `mariadb/storage/maria/ha_maria.cc:ha_maria::get_auto_increment()` compute
  grouped values by looking up the last row in the current key prefix.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()`
  dispatches grouped later-in-key tables to
  `mylite_read_grouped_auto_increment()` and reserves a single generated value.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_read_grouped_auto_increment()`
  reads live index entries for the grouped key prefix, selects the maximum
  matching key tuple for the current prefix, fetches the selected row, and
  decodes the autoincrement value from the stored MariaDB record image.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` still preserves
  durable table-local generated autoincrement pages for first-key behavior,
  but grouped allocation ignores that scalar state and recomputes from live
  prefix entries.

## Compatibility Impact

Grouped generated values remain MyISAM/Aria-style per-prefix values for routed
tables, including explicit `ENGINE=InnoDB` declarations that resolve to MyLite.
Rolled-back grouped rows no longer influence the next generated value for that
prefix because they are absent from live row/index visibility after rollback.
This intentionally differs from first-key table-local gap preservation, which
matches MariaDB/InnoDB-style persistent counter behavior.

## Affected MariaDB Subsystems

- SQL-layer `handler::update_auto_increment()` singleton grouped intervals.
- MyLite handler grouped `AUTO_INCREMENT` allocation.
- MyLite transaction and savepoint rollback of row/index visibility.
- MyLite engine-name routing for explicit `ENGINE=InnoDB` declarations.

## Design

Keep grouped allocation as a live-prefix maximum lookup:

1. create a routed grouped table with the autoincrement column later in the
   primary key;
2. insert committed rows in more than one prefix;
3. insert grouped generated rows inside an active transaction and roll them
   back;
4. insert again into the same prefix and assert that the generated value is
   reused from the live maximum;
5. repeat the same check for a nested savepoint rollback and a prepared insert
   followed by transaction rollback;
6. close and reopen the file to verify that durable catalog and live row/index
   state still produce the expected next per-prefix values.

No production storage change is expected if live row-state rollback and grouped
index-entry filtering are already correct.

## File Lifecycle

No file-format or companion-file change is introduced. The primary `.mylite`
file remains the durable asset, with existing MyLite transaction and statement
journals remaining transient lifecycle files.

## DDL Metadata Routing Impact

No DDL metadata format change is introduced. The test uses explicit
`ENGINE=InnoDB` to keep alias routing covered while checking grouped
autoincrement behavior through MyLite.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary direct and prepared SQL execution against MyLite-routed tables.

## Storage-Engine Routing

The test targets a file-backed MyLite-routed table requested as `InnoDB`.
Native InnoDB remains absent from the default embedded profile.

## Build, Size, And Dependencies

No dependency, binary-size, or build-profile change is introduced.

## Wire-Protocol Or Integration-Package Impact

None. This is embedded handler/storage behavior coverage only.

## License Or Dependency Impact

None.

## Test Plan

- Add embedded storage-engine coverage for grouped later-in-key generated rows
  rolled back by direct transaction rollback, nested savepoint rollback, and a
  prepared insert followed by transaction rollback.
- Assert per-prefix generated values after rollback and after close/reopen.
- Run the focused storage-smoke build and test target.
- Run the storage-smoke preset, syntax checks, clang-format diff, and
  `git diff --check`.

## Acceptance Criteria

- Rolled-back grouped generated rows are not visible.
- The next grouped generated value resumes from the live maximum for the same
  prefix after direct transaction rollback.
- The same live-prefix behavior holds after savepoint rollback.
- The same live-prefix behavior holds after prepared execution followed by
  transaction rollback.
- Close/reopen preserves the resulting per-prefix allocation state.
- First-key table-local rollback gap behavior remains unchanged.

## Risks And Unresolved Questions

- This does not add storage-level B-tree prefix lookup; durable append-tail
  fallback and volatile grouped allocation still scan their narrowed prefix
  entry streams.
- Broader trigger, view, and exhaustive expression-error grouped matrices
  remain separate work.
