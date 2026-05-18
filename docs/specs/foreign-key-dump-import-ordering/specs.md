# Foreign-Key Dump Import Ordering

## Goal

Cover a dump-style foreign-key data import where the session disables
`foreign_key_checks`, inserts child rows before matching parent rows, and then
re-enables checks.

## Non-Goals

- Do not support creating foreign-key metadata against missing parent tables.
- Do not add retrospective validation when `foreign_key_checks` is restored to
  `1`.
- Do not cover `mysqldump` conditional comments or dump-client formatting.
- Do not broaden foreign-key action support beyond the currently documented
  MyLite subset.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:5018-5027` defines `foreign_key_checks` as a session
  bit and documents reloads where data arrives in a different order from
  parent/child relationships. It also states that re-enabling the variable does
  not retrospectively validate inconsistent rows.
- `mariadb/sql/sql_priv.h:77-80` defines `OPTION_NO_FOREIGN_KEY_CHECKS` as the
  THD flag for importing tables in a wrong order.
- `mariadb/storage/mylite/ha_mylite.cc:935-938` reads that THD flag, and
  `ha_mylite::write_row()`, `update_row()`, and `delete_row()` skip MyLite FK
  child/parent checks and actions while it is set.

## Compatibility Impact

This improves compatibility for dump/import workflows that load data in an
order that would temporarily violate foreign-key constraints. Existing orphaned
rows inserted while checks are disabled remain visible when checks are restored,
matching MariaDB's documented non-retrospective behavior. New violating writes
after checks are restored still fail.

## Affected MariaDB Subsystems

- Session system variables and THD option bits.
- MyLite handler row insert/update/delete FK enforcement.
- SQL fixture import coverage.

## Design

Add a dedicated SQL fixture that:

1. Disables `foreign_key_checks`.
2. Creates parent and child MyLite-routed tables with a supported FK action
   pair.
3. Inserts child rows before parent rows, including one intentionally unmatched
   child row.
4. Inserts the matching parent row and re-enables `foreign_key_checks`.

The storage smoke test imports that fixture, verifies metadata and rows,
asserts that new orphan inserts fail after checks are restored, verifies
supported update/delete actions still apply to consistent rows, and repeats the
important assertions after close/reopen.

## DDL Metadata Routing Impact

No new metadata record type is introduced. Parent and child definitions still
use existing catalog-backed table and FK metadata.

## Single-File And Embedded Lifecycle

All durable definitions, rows, indexes, and FK metadata remain in the primary
`.mylite` file. The test keeps sidecar and runtime-schema-directory checks in
place across close/reopen.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

The behavior applies to supported MyLite-routed InnoDB declarations. Native
InnoDB remains absent from the default embedded profile.

## Wire-Protocol Or Integration-Package Impact

None.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Add `foreign-key-dump-import.sql`.
- Add a storage-engine smoke test that imports the fixture and validates FK
  enforcement after checks are restored.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Child rows can be imported before parent rows while
  `foreign_key_checks=0`.
- Rows left inconsistent while checks are disabled survive re-enable and
  close/reopen without retrospective validation.
- New child-row violations fail after checks are restored.
- Supported update/delete actions still apply to consistent imported rows.
- No durable sidecars or runtime schema directories appear.

## Implementation Status

Implemented in storage-engine smoke coverage with
`foreign-key-dump-import.sql`.

## Risks And Unresolved Questions

- Full dump-client compatibility with conditional comments, lock statements,
  and multi-schema ordering remains planned.
