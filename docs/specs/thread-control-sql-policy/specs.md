# Thread Control SQL Policy

## Problem Statement

MyLite's core API is an embedded, directory-owned library, not a daemon with a
server thread inventory or a process lifetime controlled by SQL. MariaDB's
`KILL` and `SHUTDOWN` statements target server connection threads and server
shutdown state. Letting those statements reach the embedded server creates
ambiguous behavior for same-process handles and ownerless peer processes.

This slice rejects thread-control SQL at the existing server-surface policy
gate for both direct execution and prepared statements.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:15196` parses `KILL` into `SQLCOM_KILL` and supports
  connection, query, query-id, user, hard, and soft forms.
- `mariadb/sql/sql_yacc.yy:15247` parses `SHUTDOWN` into `SQLCOM_SHUTDOWN` and
  accepts `WAIT FOR ALL SLAVES`.
- `mariadb/sql/sql_parse.cc:5508` dispatches `SQLCOM_KILL` to `sql_kill()` or
  `sql_kill_user()` after evaluating the supplied id or user.
- `mariadb/sql/sql_parse.cc:5532` dispatches `SQLCOM_SHUTDOWN` to `kill_mysql()`
  in daemon builds and reports an embedded-server unsupported diagnostic in
  embedded builds.
- `mariadb/sql/sql_parse.cc:2356` maps the protocol `COM_PROCESS_KILL` command
  to `sql_kill()`. MyLite's core SQL API has no wire-protocol command surface,
  so this slice scopes only SQL text handled by `mylite_exec()` and
  `mylite_prepare()`.
- `mariadb/sql/mysqld.cc:1767` closes listener sockets and connection state
  during server shutdown, confirming that shutdown is a daemon lifecycle path.

## Design

Extend `is_unsupported_server_surface_sql()` with a first-token policy for
`KILL` and `SHUTDOWN`. The existing SQL policy tokenizer already skips ordinary
comments, honors MariaDB executable comments, and ignores quoted string
literals, so this remains a narrow statement-family gate rather than a parser.

The rejection is global, not ownerless-only:

- same-process embedded handles do not expose a MariaDB server thread-control
  contract;
- ownerless peers coordinate through directory-owned process slots, locks,
  page-version pins, and redo/page visibility state, not SQL commands that kill
  another connection thread;
- future wire-protocol integration may add explicit protocol diagnostics around
  the core API, but it must not weaken the core library boundary.

## Compatibility Impact

Applications that issue daemon administration statements such as `KILL` or
`SHUTDOWN` must use a server integration layer. Core `libmylite` returns
`MYLITE_ERROR` with the stable server-surface diagnostic before MariaDB dispatch
for direct and prepared SQL.

Ordinary application SQL containing `KILL` or `SHUTDOWN` inside quoted string
literals remains valid.

## Database Directory And Native Storage Impact

No directory layout or native storage behavior changes. The slice prevents
thread-control statements from entering MariaDB server lifecycle paths; table
DDL, DML, transactions, and native engine operations continue unchanged.

## Test Plan

- Extend `libmylite.embedded-server-surface-policy` to reject direct `KILL`,
  `KILL QUERY`, `KILL CONNECTION`, `KILL SOFT USER`, executable-comment `KILL`,
  `SHUTDOWN`, and `SHUTDOWN WAIT FOR ALL SLAVES`.
- Cover prepared `KILL`, `KILL QUERY`, `SHUTDOWN`, and
  `SHUTDOWN WAIT FOR ALL SLAVES`.
- Keep quoted string literals containing the same words executable.
- Run the focused server-surface policy test, ownerless SQL suites, ownerless
  stress, format check, and diff whitespace checks.

## Acceptance Criteria

- `KILL` and `SHUTDOWN` fail through `server-owned SQL surface is not supported
  by MyLite` in direct execution and preparation.
- MySQL/MariaDB executable comments cannot bypass the rejection.
- String literals containing `KILL` or `SHUTDOWN` do not trip the policy.
- Compatibility and ownerless concurrency docs describe the boundary without
  claiming server-thread-control support.
