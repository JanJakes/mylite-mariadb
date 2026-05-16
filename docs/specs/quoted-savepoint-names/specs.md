# Quoted Savepoint Names

Status note: this slice kept transaction modifiers out of scope. The later
[Transaction Modifier Control](../transaction-modifier-control/specs.md) slice
adds bounded direct modifier support; prepared transaction-start/completion
modifiers remain unsupported. The later
[Double-Quoted Savepoint Names](../double-quoted-savepoint-names/specs.md)
slice adds SQL-mode-aware double-quoted savepoint identifiers.

## Problem

MyLite supports direct and prepared savepoint-control statements inside bounded
file-backed row-DML transactions, but the current `libmylite` parser only
accepts simple unquoted savepoint names. MySQL/MariaDB applications can use
backtick-quoted identifiers for savepoint names, including names with spaces or
reserved words, so this remains a compatibility gap in the current
`ENGINE=InnoDB`-routes-to-MyLite transaction surface.

This slice adds backtick-quoted savepoint names to the existing direct and
prepared MyLite-owned savepoint path. It does not add handler-level savepoint
hooks, SQL-mode-sensitive double-quoted identifiers, transactional DDL,
isolation, XA, transaction modifiers, or fully transactional engine flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses savepoint control with `ident`:
  `SAVEPOINT ident`, `ROLLBACK ... TO [SAVEPOINT] ident`, and
  `RELEASE SAVEPOINT ident`.
- `mariadb/sql/sql_lex.cc:Lex_input_stream::scan_ident_delimited()` handles
  delimited identifiers by using a doubled quote character as an escaped
  literal quote inside the identifier.
- `mariadb/sql/transaction.cc:trans_savepoint()`,
  `trans_rollback_to_savepoint()`, and `trans_release_savepoint()` pass the
  parsed `LEX_CSTRING` savepoint name to the SQL transaction/savepoint layer.
- `mariadb/storage/mylite/ha_mylite.h` still advertises
  `HA_NO_TRANSACTIONS`, and the MyLite handler still lacks handler-level
  savepoint hooks. MyLite must keep savepoint execution on the bounded
  `libmylite` checkpoint path for now.

## Design

Extend the `libmylite` savepoint-control parser:

- Keep existing simple unquoted savepoint identifiers.
- Add backtick-quoted identifiers for direct and prepared
  `SAVEPOINT`, `ROLLBACK TO [SAVEPOINT]`, and `RELEASE SAVEPOINT`.
- Decode doubled backticks inside a quoted identifier into one literal
  backtick in the stored savepoint name.
- Continue to require exactly one non-empty savepoint name followed only by
  statement-ending whitespace, comments, or one trailing semicolon.
- Continue rejecting unsupported savepoint syntax before MariaDB execution.

The parsed name is stored as `std::string` before execution. The existing
checkpoint stack continues to compare stored savepoint names byte-for-byte.

## Affected Subsystems

- `packages/libmylite`: direct and prepared savepoint-control parsing.
- Embedded direct and prepared SQL tests.
- Storage-engine transaction tests.
- API, storage architecture, compatibility, roadmap, and spec docs.

## Compatibility Impact

Applications can use backtick-quoted savepoint names in the current bounded
row-DML transaction scope, including names that contain spaces or escaped
backticks. Compatibility remains partial:

- SQL-mode-sensitive double-quoted identifiers are covered by a later slice.
- Execution outside an active file-backed MyLite transaction still fails.
- Handler-level savepoint hooks and full transactional engine semantics remain
  planned.
- MEMORY/HEAP row savepoints, transactional DDL, isolation, XA, and unsupported
  transaction modifiers remain unsupported.

## DDL Metadata Routing Impact

No catalog format or DDL routing behavior changes. DDL remains rejected while a
direct transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. Quoted savepoint names reuse the
existing nested storage checkpoint stack and transaction journal behavior.

## Public API And File Format

The public C API and primary `.mylite` file format do not change. The behavior
is exposed through direct and prepared SQL text accepted by existing APIs.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not change
BLACKHOLE or MEMORY/HEAP special behavior.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future integration layers should
delegate these savepoint-control statements to `libmylite` until handler-level
savepoint hooks exist.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to a small parser helper and
tests.

## Test And Verification Plan

- Extend direct SQL policy tests for backtick-quoted names, including escaped
  backticks.
- Extend prepared-statement policy tests for backtick-quoted savepoint
  control.
- Add storage-smoke coverage proving quoted direct rollback and quoted
  prepared rollback/release over a routed `ENGINE=InnoDB` table.
- Continue rejecting missing, unterminated, chained, and unsupported savepoint
  syntax.
- Run dev, embedded, storage-smoke, transaction and prepared-statement harness
  groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct and prepared backtick-quoted savepoint names work over bounded
  file-backed MyLite row-DML transactions.
- Escaped backticks compare as part of the stored logical savepoint name.
- Unsupported quoted-name forms fail explicitly instead of falling through to
  MariaDB.
- Docs and compatibility tables no longer list all quoted savepoint names as
  unsupported.

## Risks And Unresolved Questions

- Savepoint name comparison remains byte-for-byte on the decoded identifier.
  If MariaDB applies additional collation or case folding to savepoint names,
  a later compatibility slice should align the comparison behavior.
- Double-quoted identifiers depend on SQL mode and are covered by the later
  double-quoted savepoint names slice.
