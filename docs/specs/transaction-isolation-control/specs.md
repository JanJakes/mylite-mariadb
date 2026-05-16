# Transaction Isolation Control

## Problem

MyLite now accepts bounded direct transaction starts, access modes,
autocommit controls, completion chaining, and savepoints, but it still rejects
`SET TRANSACTION ISOLATION LEVEL ...` forms. Those statements are common
connection setup SQL for MySQL/MariaDB applications and frameworks.

This slice accepts MariaDB's direct/session transaction isolation control
surface after MariaDB validates the SQL. It intentionally does not claim MyLite
storage isolation semantics; the isolation level is treated as a compatibility
control until storage locking, WAL/checkpoint, and transactional engine flags
can prove real isolation behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documentation for
  [`SET TRANSACTION`](https://mariadb.com/docs/server/reference/sql-statements/transactions/set-transaction)
  documents `ISOLATION LEVEL READ UNCOMMITTED`, `READ COMMITTED`,
  `REPEATABLE READ`, and `SERIALIZABLE`, with optional session/global scope.
- `mariadb/sql/sql_yacc.yy:17460-17521` parses transaction
  characteristics as either access mode, isolation level, or one access mode
  plus one isolation level in either order.
- `mariadb/sql/sql_yacc.yy:17486-17521` maps isolation levels to the
  `transaction_isolation` system variable through the parsed option scope.
- `mariadb/sql/sys_vars.cc:4480-4507` defines `tx_isolation` and
  `transaction_isolation`, with checks that reject changing the one-shot next
  transaction isolation while an active multi-statement transaction is already
  underway.
- `mariadb/sql/sql_class.h:4021-4049` documents `THD::tx_isolation` as the
  current or next transaction isolation level. It resets from the session
  default at transaction completion unless a chained transaction preserves the
  active characteristics.
- `mariadb/sql/transaction.cc:48-58` resets one-shot transaction
  characteristics to the session values, and `transaction.cc:225-241` includes
  the active isolation level in transaction instrumentation.
- `mariadb/sql/sql_parse.cc:5598-5668` preserves transaction characteristics
  across `AND CHAIN` and resets one-shot characteristics after non-chained
  `COMMIT` / `ROLLBACK`.
- `mariadb/sql/set_var.cc:221-224` notes that `transaction_isolation` and
  `transaction_read_only` can be global, session, or one-shot variables rather
  than ordinary session-only variables.

## Design

- Extend direct `SET TRANSACTION` parsing to accept:
  - `SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED`
  - `SET TRANSACTION ISOLATION LEVEL READ COMMITTED`
  - `SET TRANSACTION ISOLATION LEVEL REPEATABLE READ`
  - `SET TRANSACTION ISOLATION LEVEL SERIALIZABLE`
  - the same forms under `SET SESSION TRANSACTION` and
    `SET LOCAL TRANSACTION`
  - one access mode plus one isolation level in either MariaDB-supported order.
- Keep `SET GLOBAL TRANSACTION ...` rejected in the public MyLite SQL API.
- Keep transaction isolation system-variable assignments rejected, including
  `SET transaction_isolation=...`, `SET tx_isolation=...`, and mixed `SET`
  lists targeting those variables.
- Treat accepted isolation controls as MyLite no-ops after MariaDB accepts the
  SQL. If a supported access mode is present in the same statement, continue to
  mirror the read-only/read-write state exactly as the read-only transaction
  slice already does.
- Keep prepared transaction-control statements rejected before MariaDB prepare,
  except for the existing prepared savepoint-control support.

## Affected Subsystems

- `packages/libmylite`: transaction-control SQL classification.
- Direct SQL, prepared statement, and storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, and related slice
  specs.

## Compatibility Impact

Applications can now run common direct/session transaction isolation setup SQL
without tripping MyLite's transaction policy gate. The accepted SQL affects
MariaDB session state, but MyLite storage still does not advertise InnoDB-level
isolation guarantees. Compatibility remains partial and explicit.

## DDL Metadata Routing Impact

No DDL metadata changes.

## Single-File And Embedded Lifecycle

No file-format, journal, lock, or companion-file behavior changes. Isolation
controls do not open or close MyLite storage checkpoints.

## Public API And File Format

No C API or `.mylite` file-format changes. The behavior is exposed through
`mylite_exec()` accepting additional direct transaction-control SQL.

## Storage-Engine Routing Impact

The accepted isolation controls apply to routed MyLite tables, including
`ENGINE=InnoDB` requests that resolve to MyLite, only as compatibility setup
SQL. Native InnoDB isolation, consistent snapshots, gap locks, lock waits, and
transactional engine flags remain planned.

## Wire Protocol Or Integration Impact

No wire-protocol package changes. Future protocol adapters should delegate
these controls to the core library.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to transaction-control
parser branches and tests.

## Test And Verification Plan

- Extend direct SQL transaction policy tests to accept next/session/local
  isolation controls and access-mode plus isolation combinations in both
  MariaDB-supported orders.
- Keep global transaction isolation controls, duplicate isolation
  characteristics, semicolon-chained forms, and transaction isolation variable
  assignments rejected.
- Extend prepared-statement policy tests to keep prepared isolation controls
  rejected.
- Add storage-smoke coverage proving isolation plus `READ ONLY` still applies
  the read-only access mode, and isolation plus `READ WRITE` still allows a
  writeable transaction.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct/session/local `SET TRANSACTION ISOLATION LEVEL ...` forms are accepted
  after MariaDB accepts the SQL.
- Access-mode plus isolation combinations preserve the existing MyLite
  read-only/read-write state behavior.
- Global controls, variable assignments, duplicate characteristics, prepared
  transaction-control statements, and semicolon-chained forms remain rejected.
- Docs describe this as accepted compatibility setup SQL, not implemented
  storage isolation.

## Risks And Unresolved Questions

- Accepting isolation controls may let applications assume stronger storage
  isolation than MyLite currently implements. The compatibility matrix and API
  docs must keep that limitation explicit.
- Future storage isolation work should replace the no-op treatment with shared
  transaction characteristic state rather than adding a second parser path.
