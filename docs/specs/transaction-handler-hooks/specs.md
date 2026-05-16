# Transaction Handler Hooks

Status note: the later
[Row-DML Transactions](../row-dml-transactions/specs.md) slice keeps the
autocommit handler-hook path but adds a `libmylite`-owned outer checkpoint for
direct row-DML transactions. Direct `BEGIN`, `COMMIT`, and `ROLLBACK` are now
covered for that bounded scope, and the later
[Autocommit Row-DML Transactions](../autocommit-row-dml-transactions/specs.md)
slice adds supported direct session `SET autocommit=0/1` forms, and the later
[Autocommit SET-List Control](../autocommit-set-list-control/specs.md) slice
allows one supported session autocommit assignment inside direct `SET` lists
with ordinary non-transaction assignments. Later transaction-variable slices
accept supported transaction variable `SET` forms, including duplicate
supported assignments. Savepoints, global or duplicate autocommit changes,
global transaction variables, XA, transaction modifiers, transactional DDL, and
transactional engine flags remain planned or rejected.

## Problem

MyLite now has a storage checkpoint that can restore the statement-start
header/catalog snapshot after a failed file-backed statement. The first
integration lives in `libmylite` around SQL execution, which is useful as a
bounded bridge but bypasses MariaDB's normal storage-engine transaction
boundary.

The next step is to let routed MyLite row DML participate in MariaDB's
statement transaction hook path. This slice wires the MyLite handler into
`trans_register_ha()`, `ha_commit_trans()`, and `ha_rollback_trans()` for
statement-level row mutations while still leaving explicit SQL transactions,
savepoints, and full multi-statement rollback unsupported.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:trans_register_ha()` is the storage-engine
  registration point for both statement (`all=false`) and normal transaction
  (`all=true`) lists. It is idempotent and records whether two-phase commit is
  unavailable when a handlerton lacks `prepare`.
- `mariadb/sql/handler.cc` documents that engines normally register from
  `handler::external_lock()`, and that MariaDB raises the statement
  read-write flag when `handler::ha_write_row()`,
  `handler::ha_update_row()`, or `handler::ha_delete_row()` calls
  `mark_trx_read_write()`.
- `mariadb/sql/transaction.cc:trans_commit_stmt()` calls
  `ha_commit_trans(thd, FALSE)` at successful statement end when the statement
  transaction list is non-empty.
- `mariadb/sql/transaction.cc:trans_rollback_stmt()` calls
  `ha_rollback_trans(thd, FALSE)` for failed statement cleanup when the
  statement transaction list is non-empty.
- `mariadb/sql/handler.cc:commit_one_phase_2()` calls each registered
  participant's `commit(thd, all)` callback. `ha_rollback_trans()` similarly
  calls each registered participant's `rollback(thd, all)` callback.
- `mariadb/sql/handler.h` defines `HA_NO_TRANSACTIONS` as the table flag that
  keeps a handler from advertising transactional table support. MyLite
  currently advertises this flag in `mariadb/storage/mylite/ha_mylite.h`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_register_trx()`
  registers InnoDB in both the statement list and, when needed, the normal
  transaction list. InnoDB then commits a whole transaction for `all=true` and
  ends a statement for `all=false`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::external_lock()` currently
  returns success without registering the MyLite handlerton, and
  `mylite_init_func()` does not install handlerton commit or rollback hooks.

## Design

Add MyLite handlerton `commit` and `rollback` callbacks plus a
per-connection handler context stored in `THD::ha_data[mylite_hton->slot]`.
The context owns at most one active `mylite_storage_statement` checkpoint.

`ha_mylite::external_lock()` will:

- ignore unlocks and read locks for now;
- on write locks, begin a MyLite storage statement checkpoint for the primary
  `.mylite` file unless an outer `libmylite` checkpoint is already active;
- register `mylite_hton` with `trans_register_ha(thd, false, mylite_hton, 0)`
  so MariaDB calls MyLite at statement commit or rollback;
- optionally register the normal transaction list when MariaDB is already in a
  multi-statement transaction, but the hook still commits at statement
  boundary because MyLite continues to reject user-visible transaction control
  before MariaDB execution.

The handlerton callbacks will:

- `commit(thd, all)`: commit and clear the active storage checkpoint if present;
- `rollback(thd, all)`: rollback and clear the active storage checkpoint if
  present;
- treat missing checkpoint state as success, because DDL paths and outer
  `libmylite` checkpoints may register or commit through different paths.

`libmylite` will keep its outer checkpoint for DDL and storage mutations that
do not reliably enter `external_lock()`, including `CREATE`, `ALTER`, `DROP`,
`RENAME`, and `TRUNCATE`. It will stop wrapping row-DML tokens that naturally
flow through the handler statement transaction path: `INSERT`, `UPDATE`,
`DELETE`, and `REPLACE`. `LOAD DATA` and `LOAD XML` are covered by the
file-import rejection policy until a controlled import surface exists.

The storage checkpoint API will expose a small query helper so the handler can
detect an already-active outer checkpoint for the same primary file rather than
failing with nested-checkpoint misuse.

## Supported Scope

- Statement transaction registration for write-locked routed MyLite tables.
- Handlerton commit and rollback callbacks for active MyLite storage
  checkpoints.
- Direct `libmylite` row-DML rollback through MariaDB's statement transaction
  path instead of the outer execution wrapper.
- Prepared row-DML rollback through the same handler transaction path.
- Continued DDL/checkpoint protection through the outer `libmylite` guard where
  MariaDB does not guarantee `external_lock()`.
- Continued explicit rejection of `BEGIN`, `COMMIT`, `ROLLBACK`,
  `SAVEPOINT`, `SET autocommit`, `SET TRANSACTION`, and `XA`.

## Non-Goals

- Removing `HA_NO_TRANSACTIONS` from MyLite table flags.
- Claiming multi-statement transaction compatibility for routed
  `ENGINE=InnoDB` tables.
- Savepoint registration or savepoint rollback.
- XA, two-phase prepare, commit ordering, group commit, or WAL integration.
- Durable crash-safe logical undo for an interrupted failed statement.
- Replacing the DDL outer checkpoint before DDL paths are proven to register
  consistently with MariaDB's handler transaction layer.

## Compatibility Impact

Routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and default-MyLite row
DML now follows MariaDB's statement transaction boundary for covered failed
statements. This aligns MyLite with the storage-engine contract MariaDB expects
for rollback at the end of failed statements.

Compatibility remains partial. The handler still advertises
`HA_NO_TRANSACTIONS`, and public `libmylite` entry points still reject explicit
transaction-control SQL before MariaDB execution. MyLite therefore does not yet
claim `COMMIT`, `ROLLBACK`, savepoint, isolation-level, or autocommit
compatibility.

## DDL Metadata Routing Impact

DDL catalog paths remain protected by the existing outer statement checkpoint.
This is intentional because MariaDB source comments state that plain
`CREATE TABLE` does not start a handler transaction through
`external_lock()`, while some mixed DDL/DML statements such as CTAS can.

## Single-File And Embedded-Lifecycle Impact

No new durable companion files are introduced. Handler-owned statement
checkpoints hold the existing primary-file advisory lock for the duration of a
MariaDB statement and release it through the handlerton callback. The
per-connection context is cleaned through the MyLite handlerton
`close_connection` hook.

If a statement reaches handler write paths and then fails, rollback restores the
pre-statement header/catalog snapshot. If the process crashes during the
statement or during rollback, the same limitations from statement checkpointing
remain: storage stays structurally recoverable, but logical statement undo is
not yet crash-safe.

## Public API And File-Format Impact

The public `libmylite` C API does not change. The internal
`packages/mylite-storage` API gains an active-check helper for coordinating
outer and handler-owned checkpoints. The file format does not change.

## Storage-Engine Routing Impact

All requested engines that route to the MyLite handler share the same statement
hook behavior because `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and
omitted/default MyLite tables all use `ha_mylite`.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
will inherit the same handler behavior when it drives SQL through the embedded
runtime.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to MyLite handler
transaction glue and one storage API helper.

## Test And Verification Plan

- Add storage API coverage for active checkpoint detection.
- Update storage-smoke row-DML rollback coverage so direct `INSERT`, `UPDATE`,
  and representative direct/prepared/select-source `REPLACE` paths prove
  rollback through the handler transaction hook, not the outer `libmylite`
  wrapper. `DELETE` remains a follow-up path because it needs separate failure
  shapes; LOAD statements are explicitly rejected as file import.
- Add prepared row-DML rollback coverage in the storage-engine smoke tests.
- Keep transaction-control rejection tests passing.
- Keep DDL rollback and sidecar lifecycle tests passing.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, the
  statement-rollback and transaction harness groups, and whitespace checks.

## Acceptance Criteria

- MyLite installs handlerton commit, rollback, and connection cleanup hooks.
- Write-locked routed MyLite tables register with MariaDB's statement
  transaction list.
- Covered failed row-DML statements roll back through
  `ha_rollback_trans(thd, false)`.
- Covered successful row-DML statements commit through
  `ha_commit_trans(thd, false)`.
- Outer `libmylite` checkpoints still cover DDL paths that do not reliably
  register through `external_lock()`.
- Explicit transaction-control SQL remains rejected and docs do not imply full
  transaction support.

## Risks And Unresolved Questions

- Registering while keeping `HA_NO_TRANSACTIONS` is a deliberate intermediate
  state. It should keep user-visible transaction claims conservative, but it
  may expose MariaDB paths that expect registered engines to be fully
  transactional.
- Some DDL/DML hybrids may still use both the outer checkpoint and handler
  registration. The active-check helper must make nested ownership explicit and
  harmless.
- Multi-statement transaction behavior remains autocommit-like internally until
  MyLite has a real transaction log and can remove `HA_NO_TRANSACTIONS`.
