# Ownerless Server-Surface Pressure Policy

## Problem

The ownerless active-reader pressure limiter returns `MYLITE_BUSY` for
supported write classes when retained page-version WAL reaches the configured
soft cap. Server-owned SQL surfaces are a different class: they are unsupported
in MyLite's embedded directory-owned runtime and must keep their explicit
policy diagnostics under pressure. Otherwise CI and application logs could
misclassify daemon/global operations as retryable ownerless write pressure.

Existing pressure-order coverage included table-admin SQL, `LOCK TABLES`,
`FLUSH TABLES ... WITH READ LOCK`, host-file import, event/scheduler SQL,
tablespace detach/import, table storage options, and sequence SQL. It did not
prove representative non-event server surfaces such as process control,
plugins, grants, binlog replay, logs, and query cache under retained-WAL
pressure.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`.
- `packages/libmylite/src/database.cc:3440` and
  `packages/libmylite/src/database.cc:3628` call
  `reject_unsupported_sql_policy()` before ownerless execution or prepared
  statement allocation.
- `packages/libmylite/src/database.cc:3471` applies ownerless page-log pressure
  handling after unsupported SQL policy rejection for direct execution.
- `packages/libmylite/src/database.cc:4419` collects the server-surface policy
  families, including account/event, plugin, replication/binlog, process
  control, query-log, query-cache, and server-variable statements.
- MariaDB routes `KILL` and `SHUTDOWN` through server-global control paths in
  `mariadb/sql/sql_parse.cc:5508` and `mariadb/sql/mysqld.cc:1725`.
- MariaDB executes `INSTALL PLUGIN` and SQL `BINLOG` through
  `mariadb/sql/sql_parse.cc:5856`.
- MariaDB executes `GRANT` through the parsed SQL command path in
  `mariadb/sql/sql_parse.cc:5948`, with ACL table/runtime machinery under
  `mariadb/sql/sql_acl.cc`.
- MariaDB query-cache variables resize global query-cache state in
  `mariadb/sql/sys_vars.cc:3424`; logging variables and `FLUSH LOGS` affect
  global log facilities in `mariadb/sql/sys_vars.cc:5454` and
  `mariadb/sql/sql_reload.cc:160`.

## Design

Extend `test_ownerless_active_reader_pressure_limit_blocks_write_classes()` at
the existing retained-WAL pressure point. Assert that representative
server-surface statements return the MyLite `server-owned SQL surface` policy
error, not `MYLITE_BUSY`, while a repeatable-read ownerless reader keeps the
page-version WAL at the configured soft cap.

The focused set covers:

- process and thread control: `KILL`, `SHUTDOWN`, `SHOW PROCESSLIST`;
- account/ACL mutation: `GRANT`;
- plugin mutation: `INSTALL PLUGIN`;
- binlog/replication: SQL `BINLOG`, `RESET MASTER`;
- global logs and query-cache state: `SET GLOBAL general_log`, `FLUSH LOGS`,
  `SET query_cache_type`, and `RESET QUERY CACHE`;
- prepared-statement policy rejection for representative `KILL`, `BINLOG`, and
  logging/query-cache assignments before statement allocation.

This slice is not a full duplicate of
`embedded_server_surface_policy_test.c`; that test remains the broad
server-surface matrix. The pressure test only proves ownerless pressure-order
precedence for representative non-event families.

## Impact

- MySQL/MariaDB compatibility: MyLite continues to reject daemon/global
  server-owned surfaces explicitly in the embedded core library.
- Database directory lifecycle: no new durable layout or storage paths.
- Native storage: no supported ownerless write behavior changes.
- Public API: no API changes.
- Binary size and dependencies: no production code or dependency changes.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` under `php-embedded-prod`.
- Run direct ownerless SQL case
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_active_reader_pressure_limit_blocks_write_classes`.
- Run production format and whitespace checks.

## Acceptance Criteria

- Representative server-owned SQL surfaces return the explicit policy error
  under active-reader retained-WAL pressure.
- Prepared representative server-owned SQL fails before statement allocation
  under the same pressure.
- Supported DML/DDL pressure assertions still return `MYLITE_BUSY`, so the
  slice proves ordering rather than weakening pressure throttling.

## Risks

This slice does not expand SQL-level table-lock fault injection. Prior
investigation showed explored SQL shapes stop before the ownerless table-wait
callback, so that remains a separate planned gap until a reachable native path
is proven.
