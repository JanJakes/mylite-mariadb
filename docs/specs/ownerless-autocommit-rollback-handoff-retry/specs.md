# Ownerless Autocommit Rollback-Handoff Retry

## Problem

The WordPress ownerless application gate intermittently lost a peer
`REPLACE`: the call returned failure while a concurrent WordPress transaction
was rolling back, and the peer row retained its previous value. Capturing the
MariaDB diagnostic before the gate's verification query showed errno `1213`.

The conflict is introduced by MyLite's cross-process ownerless coordination.
The autocommit statement waits while the peer owns the affected InnoDB page;
after native row undo, the rollback process and waiter can briefly form an
ownerless external-wait/page-write cycle. MariaDB correctly abandons the whole
statement, but exposing that implementation-only cycle makes otherwise
independent WordPress option traffic fail nondeterministically.

## Base And Source Findings

The implementation remains based on MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  bridges external record waits and page-write ownership to the shared
  ownerless registry.
- `mariadb/storage/innobase/trx/trx0roll.cc` performs native rollback before
  MyLite publishes terminal rollback state and releases page ownership.
- `packages/libmylite/src/database.cc` already performs bounded whole-statement
  retry for narrowly classified ownerless failures and provides deadlock
  rollback cleanup.
- A deterministic unsafe-hook test pauses rollback at
  `rollback-after-native-row-undo`. A peer autocommit `REPLACE` on a different
  row in the same InnoDB table then reproduces errno `1213` without the fix.
- The existing explicit two-process row-deadlock test and injected
  `record-wait-publish-deadlock` storage test distinguish ordinary MariaDB or
  published lock-graph deadlocks from the rollback-handoff coordination
  cycle.

## Design

InnoDB records a thread-local provenance bit only when an ownerless external
wait or ownerless page-write acquisition returns the deadlock result. The
embedded adapter consumes and clears that bit with the MariaDB diagnostic.
Each initial direct or prepared execution also clears stale provenance before
entering MariaDB.

LibMyLite retries at most once when all of these conditions hold:

- the handle is ownerless read/write;
- MariaDB returned errno `1213`;
- InnoDB tagged the failure as an ownerless external-wait or page-write
  coordination deadlock;
- the statement began outside an explicit transaction; and
- the top-level command is `DELETE`, `INSERT`, `REPLACE`, or `UPDATE`.

Before the retry, MyLite finishes MariaDB's deadlock rollback cleanup and
refreshes ownerless visibility. Direct SQL, ownerless deferred prepared text,
and native MariaDB prepared execution use the same policy. A failed second
attempt follows the ordinary error path; there is no retry loop.

The policy deliberately does not tag every ownerless hook result. Table/record
wait-publication fault injection still exposes `1213`, timeouts and lock-table
capacity errors retain their native diagnostics, and explicit transactions
remain application-visible deadlock victims. This prevents a logical
multi-statement transaction from being replayed implicitly.

## Compatibility And Lifecycle Impact

SQL syntax, public ABI, database-directory layout, shared-memory format,
page-version WAL format, and native InnoDB files are unchanged. The only
observable difference is that an autocommit DML statement can complete after
one internal retry when its first failure is proven to come from the scoped
ownerless coordination cycle.

The retry is safe for the admitted commands because MariaDB has rolled back
the failed implicit transaction before MyLite re-executes the whole statement.
Affected rows, last insert ID, prepared result rows, and final diagnostics come
from the successful retry or final failed attempt.

## Test And Verification Plan

- Run the deterministic rollback-handoff test for direct `REPLACE`, ownerless
  prepared-text `REPLACE`, and native prepared `REPLACE ... RETURNING`.
- Run the existing two-process explicit row-deadlock test.
- Run the ownerless storage regression, including the injected record-wait
  deadlock that must remain visible as errno `1213`.
- Run the complete hook-enabled and production CTest suites, all weighted SQL
  shards, the WordPress application gate, and the workload, randomized, and
  pressure release gates.
- Run production build-policy, formatting, clang-tidy, linked-bundle, Linux
  filesystem, macOS/APFS, and Windows/NTFS gates.

## Acceptance Criteria

- All three deterministic rollback-handoff execution modes commit the peer
  value and retain the rolled-back transaction value across ownerless reopen,
  forced shared-state rebuild, and native storage validation.
- A genuine explicit transaction deadlock still produces exactly one victim.
- Injected record-wait publication deadlock, timeout, full, and coordination
  error contracts remain unchanged.
- The retry is limited to one complete implicit DML attempt and cannot replay
  an explicit transaction.
- WordPress peer traffic no longer intermittently reports the rollback-handoff
  errno `1213`.

