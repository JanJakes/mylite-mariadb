# Transactional Engine Flags

## Problem

MyLite now has bounded row-DML transaction, statement rollback, savepoint, and
active-transaction recovery coverage, including routed MEMORY/HEAP volatile row
snapshots. The storage engine still advertises `HA_NO_TRANSACTIONS` from
`ha_mylite::table_flags()`, which leaves MariaDB treating MyLite tables as
non-transactional for SQL-layer capability checks even though the handlerton has
commit, rollback, and savepoint callbacks.

This creates a concrete compatibility mismatch: `CREATE TABLE ...
TRANSACTIONAL=1` over a MyLite-routed table is reported as an unsupported table
option, and SQL code paths that ask `handler::has_transactions()` still see
MyLite as non-transactional.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h` defines `HA_NO_TRANSACTIONS` as the table flag for
  engines that do not support transactions.
- `mariadb/sql/handler.h:handler::has_transactions()` returns false when
  `HA_NO_TRANSACTIONS` is present, and `has_transaction_manager()` requires
  both absence of `HA_NO_TRANSACTIONS` and rollback support.
- `mariadb/sql/handler.h:handler::has_rollback()` is driven by the
  handlerton-level `HTON_NO_ROLLBACK` flag. MyLite does not set
  `HTON_NO_ROLLBACK`.
- `mariadb/sql/sql_table.cc:prep_create_table()` warns about the
  `TRANSACTIONAL` table option when `handler::has_transactional_option()` is
  false.
- `mariadb/sql/sql_show.cc:store_create_info()` comments out explicit
  `TRANSACTIONAL=` table options when `has_transactional_option()` is false.
- `mariadb/sql/temporary_tables.cc:open_temporary_table()` marks user temporary
  tables as `TRANSACTIONAL_TMP_TABLE` when `has_transaction_manager()` is true.
  MyLite already identifies both transactional and non-transactional user
  temporary table shares and keeps their volatile rows outside rollback
  snapshots.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::table_flags()`
  does not advertise `HA_NO_TRANSACTIONS`. MyLite-routed `ENGINE=InnoDB` should
  resolve to MyLite storage while still looking transactional to MariaDB SQL
  capability checks.

## Design

Remove `HA_NO_TRANSACTIONS` from `ha_mylite::table_flags()`.

Leave `HTON_NO_ROLLBACK` unset, as it already is, and do not add
`HA_CRASH_SAFE`. MyLite has bounded rollback-journal and transaction-journal
coverage, but the roadmap still lists WAL/checkpoints, isolation, and broader
recovery work as planned. This slice should align MariaDB's transaction
capability gate with the bounded behavior MyLite implements without expanding
the durability claim.

Do not set `HTON_TRANSACTIONAL_AND_NON_TRANSACTIONAL`. MyLite does not yet have
a per-table non-transactional mode. Explicit `TRANSACTIONAL=1` should stop
being reported as unsupported for MyLite-routed tables; broader policy for
`TRANSACTIONAL=0` compatibility can remain a later table-option slice.

## Supported Scope

- MyLite tables report as transactional to `handler::has_transactions()` and
  `handler::has_transaction_manager()`.
- Explicit `TRANSACTIONAL=1` on MyLite-routed table creation no longer produces
  the unsupported `TRANSACTIONAL` option warning. Existing routed-engine
  fallback warnings, such as native InnoDB not being registered, remain allowed.
- `SHOW CREATE TABLE` for a MyLite-routed table with explicit
  `TRANSACTIONAL=1` shows the option normally rather than commenting it out as
  unsupported.
- Existing user temporary table lifecycle remains compatible with MyLite's
  volatile store and rollback-exclusion rules.

## Non-Goals

- Full InnoDB transaction semantics.
- Marking MyLite as crash-safe with `HA_CRASH_SAFE`.
- `HTON_TRANSACTIONAL_AND_NON_TRANSACTIONAL` or real per-table
  `TRANSACTIONAL=0` behavior.
- Durable transactional DDL.
- Real storage isolation, WAL/checkpoints, XA, replication, or binlog behavior.

## Compatibility Impact

This improves compatibility for application DDL that assumes an InnoDB-like
transactional engine, including explicit `TRANSACTIONAL=1` table options on
MariaDB. The compatibility status remains partial because MyLite transaction
support is still bounded to documented row-DML, statement, savepoint, volatile
row, and recovery paths.

## File-Lifecycle Impact

No storage format, durable sidecar, or file lifecycle changes are introduced.
The change only adjusts MariaDB SQL-layer capability flags for the existing
MyLite handler.

## Test And Verification Plan

- Extend storage-engine smoke coverage with a MyLite-routed
  `ENGINE=InnoDB TRANSACTIONAL=1` table.
- Assert that creation succeeds without an unsupported `TRANSACTIONAL` warning.
- Assert that `SHOW CREATE TABLE` exposes `TRANSACTIONAL=1` without the
  unsupported-option comment wrapper.
- Keep explicit temporary table transaction tests passing so transactional
  temporary table classification does not accidentally pull user temporary rows
  into rollback snapshots.
- Run focused storage-engine and transaction harness groups.

## Acceptance Criteria

- `ha_mylite::table_flags()` no longer includes `HA_NO_TRANSACTIONS`.
- `CREATE TABLE ... ENGINE=InnoDB TRANSACTIONAL=1` under MyLite routes to
  MyLite metadata and produces no unsupported `TRANSACTIONAL` option warning.
- `SHOW CREATE TABLE` keeps explicit `TRANSACTIONAL=1` uncommented for routed
  MyLite tables.
- Existing transaction, volatile row, and temporary-table rollback tests pass.
