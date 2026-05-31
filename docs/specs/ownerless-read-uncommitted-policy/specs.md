# Ownerless Read-Uncommitted Policy

## Problem

Ownerless cross-process reads are safe because committed page visibility is
published only after the ownerless commit path writes page-version records,
flushes native pages, and advances durable visibility state. MariaDB/InnoDB
`READ UNCOMMITTED` is different: it allows dirty, non-consistent reads. MyLite
does not have a cross-process dirty-page exposure protocol, and exposing that
isolation level in ownerless mode would silently promise behavior the current
page-version design does not prove.

## Source Findings

- `mariadb/storage/innobase/include/trx0trx.h` defines
  `TRX_ISO_READ_UNCOMMITTED` as dirty read behavior where non-locking
  `SELECT`s do not use an earlier consistent version of a record.
- The ownerless page-version read path intentionally resolves committed page
  images at a visible LSN and keeps uncommitted local writes process-local.
- Existing ownerless transaction coverage proves `READ COMMITTED`,
  `REPEATABLE READ`, and `SERIALIZABLE` shapes, including read-committed
  refresh, repeatable snapshots, consistent-snapshot pins, serializable read
  locks, and a bounded write-skew candidate.

## Design

Reject ownerless `READ UNCOMMITTED` isolation requests before MariaDB dispatch.
Reject ownerless `tx_isolation` and `transaction_isolation` system-variable
assignments entirely, including `SET STATEMENT`, because those assignments can
use expressions and would otherwise bypass the ownerless transaction-isolation
tracker. The supported ownerless isolation surface remains direct
`SET [SESSION|LOCAL] TRANSACTION ISOLATION LEVEL ...` statements for
`READ COMMITTED`, `REPEATABLE READ`, and `SERIALIZABLE`.

The policy covers:

- `SET [SESSION|LOCAL] TRANSACTION ISOLATION LEVEL READ UNCOMMITTED`;
- `SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED`;
- `tx_isolation` and `transaction_isolation` assignments, including `@@`
  qualified forms; and
- `SET STATEMENT tx_isolation = ... FOR ...`.

The policy is scoped to ownerless coordination. Ordinary exclusive embedded
opens still inherit MariaDB/InnoDB `READ UNCOMMITTED`.

## Scope And Non-Goals

In scope:

- Fail closed for unproven cross-process dirty-read isolation.
- Fail closed for untracked isolation system-variable assignment forms.
- Preserve ownerless `READ COMMITTED`, `REPEATABLE READ`, and `SERIALIZABLE`.
- Keep ordinary exclusive mode behavior unchanged.

Out of scope:

- Designing a cross-process dirty-read protocol.
- Claiming the complete isolation-level matrix.
- External MariaDB/RQG transaction-oracle stress.

## Compatibility Impact

Ownerless mode becomes stricter but less misleading: applications that require
dirty reads receive an explicit MyLite policy error instead of an accidental
committed-only approximation, and applications that use isolation system
variables receive an explicit ownerless unsupported-surface error instead of
silently desynchronizing MariaDB session state from MyLite's ownerless
snapshot policy. This keeps the ownerless compatibility matrix honest while
retaining inherited MariaDB behavior outside ownerless mode.

## Directory And Lifecycle Impact

No directory layout changes. The policy prevents SQL from entering an
unsupported ownerless read path; existing committed page-version WAL,
checkpoint, and shared-memory state are unchanged.

## Native Storage Impact

No native storage format changes. Exclusive mode still uses native InnoDB
`READ UNCOMMITTED`; ownerless mode rejects it before dispatch.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `read-uncommitted-policy` in `embedded-dev`.
- Build and run focused `read-uncommitted-policy` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- Ownerless direct `SET TRANSACTION ... READ UNCOMMITTED` and isolation system
  variable assignments fail with a MyLite policy error.
- Ownerless `READ COMMITTED`, `REPEATABLE READ`, and `SERIALIZABLE` continue to
  work.
- Ordinary exclusive opens still accept MariaDB `READ UNCOMMITTED`.
- Existing ownerless SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- This does not implement dirty-read compatibility across processes. It records
  that as unsupported until a future design can expose uncommitted peer pages
  without corrupting recovery or page-version visibility.
