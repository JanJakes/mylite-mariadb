# Transaction Lifecycle Rejection Matrix

## Goal

Make the remaining unsupported transaction lifecycle forms explicit in the
storage smoke coverage: consistent snapshots, release completion, release
completion-type defaults, and XA.

## Scope

Add direct and prepared coverage proving these forms fail with stable MyLite
transaction-control diagnostics:

- `START TRANSACTION WITH CONSISTENT SNAPSHOT`;
- `COMMIT` / `ROLLBACK` release forms;
- `completion_type=RELEASE` and `completion_type=2`;
- representative XA lifecycle forms including start, end, prepare, commit,
  rollback, and recover.

## Non-Goals

- Implement consistent snapshots, connection release semantics, or XA.
- Change supported `AND CHAIN`, `AND NO CHAIN`, or `NO RELEASE` behavior.
- Add durable transactional DDL or handler-level transaction flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8330-8343` parses `START TRANSACTION` and records
  start options, including consistent snapshots.
- `mariadb/sql/transaction.cc:214-222` starts a consistent snapshot through
  handler hooks after marking the transaction active.
- `mariadb/sql/sql_yacc.yy:18283-18318` parses completion chain and release
  modifiers, rejecting `AND CHAIN RELEASE`.
- `mariadb/sql/sql_parse.cc:5593-5623` and `5631-5667` compute completion
  chain/release behavior from explicit modifiers and `completion_type`.
- `mariadb/sql/sys_vars.cc:957-961` defines `completion_type` values
  `NO_CHAIN`, `CHAIN`, and `RELEASE`.
- `mariadb/sql/sql_yacc.yy:18660-18685` parses XA start, end, prepare,
  commit, rollback, and recover statements.
- `mariadb/sql/xa.cc:441-888` implements XA transaction state transitions,
  recovered-XID cache interaction, and handler two-phase completion.
- `packages/libmylite/src/database.cc:4007-4009` rejects XA before MariaDB
  execution or prepare.
- `packages/libmylite/src/database.cc:4034-4067` rejects unsupported
  transaction start options, including consistent snapshots.
- `packages/libmylite/src/database.cc:4070-4124` rejects release completion
  while keeping `NO RELEASE` supported.
- `packages/libmylite/src/database.cc:4473-4530` rejects unsupported
  `completion_type` values including `RELEASE` and `2`.

## Design

No production behavior change is required. MyLite already rejects these forms
through the first-party SQL policy scanner. This slice adds storage-smoke
coverage so the unsupported boundary is visible in the same routed
`ENGINE=InnoDB` transaction test that proves the supported bounded lifecycle
forms.

Consistent snapshots remain unsupported because they rely on handler snapshot
hooks and real isolation guarantees. Release completion remains unsupported
because `libmylite` owns an embedded database handle rather than a
wire-protocol connection that can be killed after completion. XA remains
unsupported because MyLite does not yet advertise transactional handler flags,
two-phase commit hooks, or recovered-XID state for its single-file storage.

## Compatibility Impact

The supported lifecycle surface is unchanged. Applications can still use
bounded direct/prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, `ROLLBACK`,
supported chain/no-chain/no-release modifiers, supported session
`completion_type`, and savepoints. Unsupported server-oriented lifecycle forms
now have explicit routed-storage coverage.

## File Lifecycle

No file-format, journal, lock, or companion-file behavior changes.

## Embedded Lifecycle And API

No public C API changes. `mylite_exec()` and `mylite_prepare()` continue to
return `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB errno, and stable
transaction-control diagnostics for these failures.

## Test Plan

- Add direct storage-smoke coverage for consistent snapshot, release
  completion, release completion defaults, and representative XA forms.
- Add prepared prepare-time storage-smoke coverage for consistent snapshot,
  release completion, and representative XA forms.
- Run the focused storage-smoke binary, the transaction compatibility harness
  group, shell syntax checks, reject-file cleanup checks, and whitespace
  checks.

## Acceptance Criteria

- Consistent snapshots fail before MariaDB execution or prepare.
- Release completion and release completion-type defaults fail before MariaDB
  mutates MyLite's mirrored transaction state.
- Representative XA statements fail before MariaDB execution or prepare.
- Existing supported bounded lifecycle controls still pass.

## Risks And Open Questions

- A future wire-protocol wrapper may need a product-level decision for
  `RELEASE` semantics. The core embedded handle should not silently close or
  poison itself to mimic server connection release.
- XA support would require a real transaction and recovery design across
  MyLite's file format, handler flags, and MariaDB's XA state machine.
