# SQL Locking Policy

## Problem

MyLite has primary-file advisory locks and busy-timeout waits, but it does not
yet implement SQL-level table locks, row locks, named locks, gap locks, or
transaction-aware lock release. Letting MariaDB accept locking SQL would imply
compatibility that MyLite cannot currently honor, especially for
`LOCK TABLES`, `SELECT ... FOR UPDATE`, and named-lock functions.

This slice makes those surfaces explicit policy rejections until real SQL lock
integration is designed.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `LOCK TABLES` and `UNLOCK TABLES` into
  `SQLCOM_LOCK_TABLES` and `SQLCOM_UNLOCK_TABLES`.
- `mariadb/sql/sql_parse.cc` routes `SQLCOM_LOCK_TABLES` and
  `SQLCOM_UNLOCK_TABLES` through server table-lock handling, not the MyLite
  primary-file lock manager.
- `mariadb/sql/sql_parse.cc` also documents locking reads with
  `SELECT {FOR UPDATE/LOCK IN SHARED MODE} SKIP LOCKED`, which require
  semantics beyond a plain MyLite read.
- `mariadb/sql/item_create.cc` registers named-lock functions such as
  `GET_LOCK` and `RELEASE_LOCK` as server-level synchronization functions.
- `mariadb/sql/handler.cc:handler::ha_external_lock()` and
  `trans_register_ha()` show where engines participate in statement and
  transaction lock lifetimes. MyLite currently has coarse primary-file
  advisory locks and statement checkpoints, not row or transaction locks.

## Scope

- Reject representative `LOCK TABLES` and `UNLOCK TABLES` statements.
- Reject `SELECT ... FOR UPDATE`.
- Reject `SELECT ... LOCK IN SHARE MODE`.
- Reject named-lock functions in `SELECT`, including `GET_LOCK` and
  `RELEASE_LOCK`.
- Cover direct execution, prepared prepare-time policy, and file-backed
  storage-engine smoke behavior.

## Non-Goals

- Implementing table locks, row locks, gap locks, named locks, or deadlock
  detection.
- Mapping MariaDB lock-wait variables to MyLite storage locks.
- Supporting harmless no-op `UNLOCK TABLES`.
- Providing lock semantics for a raw MariaDB adapter.

## Design

Extend the existing `unsupported_sql_surface_message()` policy gate with a
locking predicate:

1. Top-level `LOCK TABLE[S]` and `UNLOCK TABLE[S]` reject immediately.
2. Top-level `SELECT` scans SQL tokens outside quotes and comments for
   `FOR UPDATE`, `LOCK IN SHARE MODE`, and named-lock functions.
3. The policy returns stable MyLite diagnostics before MariaDB prepare or
   execution, with no MariaDB errno.

The tokenizer remains deliberately conservative. It avoids quoted-string false
positives for `FOR UPDATE`, but it is not a substitute for final SQL lock
semantics.

## Compatibility Impact

SQL locking moves from accidental behavior to explicit partial coverage:
unsupported locking constructs fail predictably through the public API. Real
MariaDB-compatible SQL lock semantics remain planned under the locking and
transaction roadmap slices.

## Single-File And Embedded-Lifecycle Impact

No file-format or companion-file change. Rejected statements must not create
durable sidecars or alter MyLite catalog state.

## Build, Size, And Dependencies

No dependency or build-profile change. The runtime change is a small policy
predicate and tests.

## Test And Verification Plan

- Extend direct execution tests for representative rejected locking SQL and a
  quoted `FOR UPDATE` non-match.
- Extend prepared-statement diagnostics for prepare-time locking rejection.
- Extend storage-engine smoke coverage for file-backed routed tables and
  sidecar cleanup after rejected locking SQL.
- Add the direct and prepared tests to `compat-locking`.
- Run the locking compatibility report plus format, tidy, dev, embedded-dev,
  storage-smoke-dev, and diff checks.

## Acceptance Criteria

- Direct `LOCK TABLES`, `UNLOCK TABLES`, `SELECT ... FOR UPDATE`,
  `SELECT ... LOCK IN SHARE MODE`, and named-lock functions return
  `MYLITE_ERROR` with MyLite policy diagnostics.
- Prepared `SELECT ... FOR UPDATE` rejects before MariaDB prepare.
- Quoted text containing `FOR UPDATE` does not trigger the policy.
- File-backed storage-engine tests still leave no durable sidecars.
- Docs, roadmap, compatibility matrix, and harness group descriptions mention
  explicit SQL locking policy coverage.

## Risks And Open Questions

- Some applications issue harmless `UNLOCK TABLES` defensively. Rejecting it is
  stricter than MariaDB, but safer than implying table-lock support that does
  not exist.
- The SQL scanner is representative. A later SQL-layer lock implementation
  should replace this policy gate for supported forms.
