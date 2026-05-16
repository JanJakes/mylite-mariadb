# Transaction Control Policy

Status note: this slice was the earlier fail-closed policy. The later
[Row-DML Transactions](../row-dml-transactions/specs.md) slice allows direct
`BEGIN`, `COMMIT`, and `ROLLBACK` for bounded row-DML transactions, and the
later [Autocommit Row-DML Transactions](../autocommit-row-dml-transactions/specs.md)
slice allows supported direct session `SET autocommit=0/1` forms. The later
[Autocommit SET-List Control](../autocommit-set-list-control/specs.md) slice
allows one supported session autocommit assignment inside direct `SET` lists
with ordinary non-transaction assignments. The later
[Transaction SET No-Op Control](../transaction-set-noop-control/specs.md)
slice accepts direct read-write/no-chain SET controls, and the later
[Completion Type Chain Control](../completion-type-chain-control/specs.md)
slice accepts chained completion defaults. The later
[Read-Only Transaction Control](../read-only-transaction-control/specs.md)
slice accepts bounded direct read-only access mode. Global or duplicate
autocommit changes, unsupported transaction-variable `SET` lists, unsupported
transaction modifiers, XA, and transactional DDL remain rejected; the later
[Prepared Transactional DDL Policy](../prepared-transactional-ddl-policy/specs.md)
slice makes the prepared execution path enforce the same active-transaction DDL
policy as direct SQL.

## Problem

MyLite has storage-level rollback-journal recovery for individual publication
paths, but it does not yet integrate with MariaDB's SQL transaction hooks.
Accepting `BEGIN`, `ROLLBACK`, savepoints, or `SET autocommit=0` would be
misleading for routed `ENGINE=InnoDB` workloads: MariaDB can accept the control
statement while MyLite table writes still publish outside a SQL transaction.

This slice rejected explicit SQL transaction-control surfaces through the
public MyLite SQL entry points until later work added bounded direct row-DML
transaction surfaces. Savepoints, global or duplicate autocommit changes,
unsupported transaction-variable `SET` lists, unsupported transaction
modifiers, XA, and transactional DDL remain rejected.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/storage/mylite/ha_mylite.h::table_flags()` includes
  `HA_NO_TRANSACTIONS`, so MariaDB sees MyLite tables as non-transactional.
- `mariadb/sql/handler.h` defines transaction and savepoint handlerton hooks:
  `commit`, `rollback`, `savepoint_set`, `savepoint_rollback`, and
  `savepoint_release`.
- `mariadb/sql/handler.cc::trans_register_ha()` documents the normal engine
  registration path, usually from `handler::external_lock()`.
- `mariadb/sql/handler.cc::ha_commit_trans()` and `ha_rollback_trans()` drive
  full and statement transaction boundaries.
- `mariadb/sql/transaction.cc` routes explicit `COMMIT`, `ROLLBACK`,
  savepoint, and transaction-start statements into the handler transaction
  layer.
- `docs/specs/transactions-recovery/specs.md` deliberately excludes SQL
  transaction support from the storage rollback-journal slice.

## Design

- Add a third MyLite SQL policy category for unsupported transaction-control
  SQL.
- Reject representative transaction starts, commits, rollbacks, savepoints,
  savepoint release, XA commands, `SET TRANSACTION`, and `SET autocommit`
  before MariaDB execution or prepare.
- Keep ordinary reads of transaction-related variables, such as
  `SHOW VARIABLES LIKE 'autocommit'`, allowed.
- Return stable MyLite diagnostics with no MariaDB errno so callers can
  distinguish policy rejection from MariaDB syntax or execution errors.

## Affected Subsystems

- `packages/libmylite/src/database.cc` SQL policy gate.
- Embedded direct and prepared SQL tests.
- Storage-engine smoke tests over routed MyLite tables.
- Compatibility harness grouping and roadmap/API/architecture documentation.

## Compatibility Impact

This earlier slice made SQL transaction control explicit and test-backed
instead of accidentally permissive. Later row-DML transaction work allows plain
direct `BEGIN`, `COMMIT`, and `ROLLBACK` for a bounded MyLite-owned checkpoint
scope, and later supported session autocommit forms use the same checkpoint
scope. The broader policy remains intentionally less permissive than MariaDB's
behavior with non-transactional engines because MyLite routes `ENGINE=InnoDB`
to the MyLite handler and must not imply full InnoDB rollback semantics until
the handler is transaction-aware.

## DDL Metadata Routing Impact

No DDL metadata format changes. Transaction-aware DDL rollback remains planned.

## Single-File And Embedded Lifecycle

No new durable files are introduced. The existing rollback journal continues to
protect individual storage publication paths; it is not exposed as SQL
transaction state.

## Public API And File Format

No public C API or file-format changes. `mylite_exec()` and `mylite_prepare()`
return `MYLITE_ERROR`, SQLSTATE `HY000`, and stable MyLite diagnostic text for
the newly rejected transaction-control statements.

## Storage-Engine Routing Impact

Routed table DDL and DML remain autocommit-style MyLite operations. Future work
must remove `HA_NO_TRANSACTIONS`, register the handlerton with MariaDB
transactions, and implement rollback/savepoint state before these statements can
be allowed.

## Wire Protocol Or Integration Impact

Future wire-protocol integrations over the public MyLite core should inherit
the later bounded row-DML transaction policy. A raw MariaDB adapter needs an
equivalent gate or real transaction integration before it becomes supported.

## Binary-Size Impact

No linked component is removed. The runtime change is a small policy predicate
and test coverage.

## License Or Dependency Impact

No dependency is introduced.

## Test And Verification Plan

- Extend embedded direct-SQL tests to reject representative transaction-control
  statements with MyLite diagnostics.
- Extend embedded prepared-statement diagnostics to reject transaction-control
  prepare before MariaDB.
- Extend storage-engine smoke tests to reject transaction controls around a
  routed `ENGINE=InnoDB` table and prove normal autocommit DML still works.
- Add a compatibility harness group for transaction-control policy.
- Run formatting, tidy, dev, embedded, storage-smoke, compatibility report for
  the affected group, and `git diff --check`.

## Acceptance Criteria

- `mylite_exec()` rejects representative transaction-control statements before
  MariaDB execution.
- `mylite_prepare()` rejects representative transaction-control statements
  before MariaDB prepare.
- Routed table DML remains usable after policy rejections.
- Documentation does not claim SQL rollback or savepoint support.

## Risks And Unresolved Questions

- This blocks applications that issue harmless `COMMIT` statements defensively.
  That is acceptable until MyLite can prove rollback semantics for routed
  MySQL/MariaDB workloads.
- The policy tokenizer is representative, not a full SQL parser. SQL-layer
  enforcement should replace it when raw MariaDB adapter support or true
  transaction hooks are added.
