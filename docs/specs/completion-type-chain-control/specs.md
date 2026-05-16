# Completion Type Chain Control

## Problem

MyLite now accepts direct session `completion_type=NO_CHAIN/0/DEFAULT` as a
no-op, but still rejects `completion_type=CHAIN`. MariaDB uses that session
default to make later plain `COMMIT` and `ROLLBACK` behave like
`COMMIT AND CHAIN` and `ROLLBACK AND CHAIN` unless the statement explicitly
uses `AND NO CHAIN`. MyLite already supports explicit `AND CHAIN`, so the
remaining gap is mirroring the accepted session default in direct transaction
bookkeeping.

This slice supports direct session `completion_type=CHAIN/1` for bounded
row-DML transactions. It keeps `completion_type=RELEASE/2`, global changes,
`SET STATEMENT`, prepared forms, read-only/isolation changes, XA, and DDL
inside active direct transactions unsupported at this slice point. A later
read-only transaction-control slice accepts bounded direct read-only access
mode, and a later transaction isolation-control slice accepts direct/session
`SET TRANSACTION ISOLATION LEVEL ...` forms as compatibility setup SQL without
claiming storage isolation semantics. A later transaction variable-control
slice accepts bounded transaction read-only and isolation variable assignments.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_completion_type` defines the session
  `completion_type` enum with values `NO_CHAIN`, `CHAIN`, and `RELEASE`, with
  `DEFAULT(0)`.
- `mariadb/sql/sql_yacc.yy` parses `COMMIT [WORK] opt_chain opt_release` and
  `ROLLBACK [WORK] opt_chain opt_release`. `opt_chain` is `TVL_UNKNOWN`,
  `TVL_NO`, or `TVL_YES`; `opt_release` has the same tri-state shape.
- `mariadb/sql/sql_parse.cc` computes `tx_chain` for `COMMIT` and `ROLLBACK`
  when the explicit chain modifier is `YES`, or when
  `thd->variables.completion_type == 1` and the statement did not explicitly
  say `AND NO CHAIN`.
- The same code computes `tx_release` from explicit `RELEASE`, or when
  `completion_type == 2` and the statement did not explicitly say
  `NO RELEASE`, then marks the connection killed.
- `mariadb/sql/transaction.cc` documents that implicit commit resets one-shot
  isolation and access mode but does not care about `@@session.completion_type`.

## Design

Extend the direct transaction-control parser and session state:

- Add a MyLite session flag mirroring whether direct session
  `completion_type` is currently `CHAIN`.
- Classify `SET completion_type=CHAIN` and `SET completion_type=1` as a
  supported direct transaction-control statement that updates the flag only
  after MariaDB accepts the whole `SET` statement.
- Keep `NO_CHAIN`, `0`, and `DEFAULT` mapped to the flag-off state.
- Preserve support for ordinary non-transaction assignments in the same direct
  `SET` list, and for a supported session autocommit assignment in the same
  list.
- Distinguish plain `COMMIT` / `ROLLBACK` from explicit
  `AND NO CHAIN` completions. Plain completions chain when the mirrored flag is
  on; explicit `AND NO CHAIN` never chains.
- Keep `RELEASE` and `completion_type=RELEASE/2` rejected because embedded
  `libmylite` does not expose server connection-release semantics.
- Keep prepared completion-type control rejected before MariaDB prepare.

## Affected Subsystems

- `packages/libmylite`: direct transaction-control classification and session
  transaction bookkeeping.
- Direct SQL, prepared policy, and storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, harness, and
  transaction spec docs.

## Compatibility Impact

Applications that set `completion_type=CHAIN` get MariaDB-compatible chaining
for plain direct `COMMIT` and `ROLLBACK` inside MyLite's bounded row-DML
transaction scope. Explicit `AND NO CHAIN` overrides the session default.

Compatibility remains partial: release completion defaults, isolation levels,
XA, transactional DDL, handler-level savepoint hooks, and fully transactional
engine flags remain unsupported. Bounded read-only access mode is handled by a
later slice.

## DDL Metadata Routing Impact

No DDL metadata behavior changes. DDL remains rejected while a direct MyLite
transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No file-format, journal, lock, or companion-file behavior changes. Chained
plain completion reuses the existing finish-and-open-new outer checkpoint path.

## Public API And File Format

The public C API and primary `.mylite` file format do not change. The behavior
is exposed through direct SQL execution.

## Storage-Engine Routing Impact

The behavior applies to routed durable MyLite row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not add native InnoDB
release or isolation semantics.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future protocol adapters should
delegate this session completion default to the core library or mirror the same
state before allowing plain completion statements.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to parser policy,
one session flag, and tests.

## Test And Verification Plan

- Extend direct SQL policy tests to accept `completion_type=CHAIN/1` and keep
  `RELEASE/2`, global, duplicate, prepared, `SET STATEMENT`, and
  semicolon-chained forms rejected.
- Keep prepared completion-type control rejected.
- Add storage-smoke coverage proving:
  - `SET completion_type=CHAIN` makes plain `COMMIT` open a new active
    transaction that can later roll back,
  - explicit `COMMIT AND NO CHAIN` overrides the chain default,
  - `SET completion_type=1` makes plain `ROLLBACK` open a new active
    transaction that can later roll back,
  - `SET completion_type=DEFAULT` returns plain completion to no-chain.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct session `completion_type=CHAIN/1` is accepted and mirrored after
  MariaDB accepts the `SET` statement.
- Plain direct `COMMIT` and `ROLLBACK` chain when the mirrored flag is on.
- Explicit `AND NO CHAIN` and `completion_type=NO_CHAIN/0/DEFAULT` keep or
  restore no-chain behavior.
- `RELEASE/2`, global, duplicate, prepared, statement-scoped, and
  chained-statement forms remain explicit unsupported transaction-control
  surfaces.
- Docs and compatibility tables describe chain default support without
  claiming release or isolation semantics.

## Risks And Unresolved Questions

- MyLite still uses conservative SQL scanning rather than MariaDB's parsed
  `set_var` list. Suspicious transaction-control targets remain rejected.
- Supporting `completion_type=RELEASE` would require a product decision about
  how embedded file-owned handles represent server connection release.
- Later read-only access-mode work must preserve the active access mode when
  opening a chained transaction; isolation defaults remain future work.
