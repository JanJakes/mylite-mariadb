# Ownerless Transaction-End Lock-Grant Progress

## Problem Statement

The `record-lock-grant-crash` unsafe-hook SQL selector exposes a statement-lock
ordering inversion. An autocommit writer can hold the ownerless global
statement read byte while it waits on an InnoDB row lock owned by a peer
explicit transaction. The row-lock holder then needs `COMMIT` to release the
native row lock, but `COMMIT` currently waits for the ownerless global
statement write byte held by the blocked writer. After the ownerless
statement-lock timeout, the holder fails with `MYLITE_BUSY` instead of making
native lock progress.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/lock/lock0lock.cc`
  - `mylite_ownerless_innodb_lock_reserve_record_for_grant()` performs a
    nonblocking shared-registry reservation before a native record lock is
    granted.
  - `mylite_ownerless_innodb_lock_wait_until_record_available_for_grant()`
    waits on the shared registry before grant retry paths.
  - `lock_grant()` clears native wait state and then calls
    `mylite_ownerless_innodb_lock_publish_record_bits()` before waking the
    waiting transaction.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_lock_reserve_record()` and
    `mylite_ownerless_innodb_lock_wait_until_record_available()` forward native
    record-grant decisions to MyLite's shared registry.
  - `mylite_ownerless_innodb_lock_publish_record_bit()` publishes the granted
    record bit back through MyLite's acquire-record callback.
  - `mylite_ownerless_innodb_lock_clear_transaction_wait()` clears shared wait
    entries when native wait state resets.
- `packages/libmylite/src/database.cc`
  - `acquire_ownerless_statement_locks()` currently makes `COMMIT` and full
    `ROLLBACK` for write transactions take the global ownerless statement write
    byte.
  - Autocommit table writes take the global ownerless statement read byte plus
    a table write byte, so a blocked autocommit writer can prevent the holder's
    transaction-end statement from reaching native InnoDB.
- `packages/libmylite/src/ownerless_innodb_lock_registry.cc`
  - Active and waiting table/record lock slots are already directory-owned and
    include owner IDs, transaction IDs, resources, modes, and conflict rules.

## Design

Add a small first-party registry query that answers whether the current owner
has any active shared InnoDB or ownerless page-write lock that conflicts with a
waiting peer lock. The query only observes the existing registry state under the
registry latch; it does not change lock ownership, wait ordering, or durable
layout.

For explicit transaction-end statements with local writes:

1. Keep acquiring the ownerless dictionary statement read lock.
2. Try the global ownerless statement write byte without waiting.
3. If it succeeds, keep the existing serialized path.
4. If it is blocked, allow the transaction-end SQL to proceed only when the
   shared InnoDB lock registry or page-write lock registry proves this owner
   currently blocks a peer waiter.
5. If the registry does not prove that condition, use the existing configured
   statement-lock timeout and return `MYLITE_BUSY` on failure.

This keeps ordinary transaction-end serialization unchanged while allowing the
native row-lock holder to reach `COMMIT`/full `ROLLBACK` when that is the
operation required to release a peer's native wait.

## Compatibility Impact

No public API, SQL syntax, storage format, or directory layout changes. The
observable compatibility improvement is that an explicit transaction that owns
the native row/table lock blocking a peer waiter can commit or roll back instead
of timing out behind that waiter at MyLite's statement gate.

The exception is intentionally narrower than "transaction-end writes never take
the global statement lock." If no ownerless InnoDB waiter is blocked by the
current owner, the existing global transaction-end statement serialization
remains in force.

## Directory And Lifecycle Impact

No new files or shared-memory segments are added. The implementation reads the
existing `concurrency/mylite-concurrency.shm` InnoDB lock-registry and
page-write lock-registry segments and uses the existing owner ID and generation
assigned to the current ownerless runtime.

## Native Storage Impact

The native InnoDB lock owner can now reach transaction end in the presence of a
blocked ownerless waiter. Native InnoDB still owns row/table lock release,
deadlock, and timeout semantics; MyLite only avoids adding an outer statement
gate cycle that MariaDB cannot see.

## Binary Size Impact

The change adds one small registry scan helper and one statement-lock branch.
No new dependency or build profile change is introduced.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` under
  `ownerless-test-hooks`.
- Run direct `record-lock-grant-crash`.
- Run full `crash-tail` to prove later unsafe-hook cases still progress.
- Run the ownerless hook CTest subset for cross-process SQL and primitives.
- Run the embedded ownerless SQL filter for normal-build coverage when time
  permits.
- Run `ownerless-stress`, `format-check`, and `git diff --check` before commit.

## Acceptance Criteria

- A row-lock holder's `COMMIT` no longer times out behind the blocked writer's
  ownerless global statement read byte when the holder is the shared-registry
  blocker.
- The killed after-grant writer leaves recovery-sensitive shared state while a
  live peer is open, so a third opener receives `MYLITE_BUSY`.
- No-live reopen preserves the holder commit and drops the interrupted writer
  update.
- Ordinary transaction-end SQL without a proven blocked peer keeps the existing
  global ownerless statement-lock behavior.

## Verification Results

Local verification on 2026-06-13 used `build/ownerless-test-hooks` and
`build/ownerless-stress`.

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
  passed.
- `mylite_ownerless_primitives_test` passed.
- `mylite_ownerless_cross_process_sql_test record-lock-grant-crash` passed in
  an isolated run.
- `mylite_ownerless_cross_process_sql_test record-lock-before-grant-crash`
  passed in an isolated run.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$'
  --output-on-failure` passed.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-negative-proof|ownerless-table-wait-negative-proof|ownerless-native-table-wait)$'
  --output-on-failure` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed, and the clean rerun of
  `ctest --preset ownerless-stress --output-on-failure` passed all 12 stress
  tests.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

The full hook `crash-tail` aggregate now gets past the previous
`record-lock-grant-crash` blocker and fails later at index 37,
`test_crashed_foreign_key_dictionary_ddl_recovers_constraint`, where a recovered
child row with primary key `2` is already present before the test's expected
post-recovery insert. The same FK crash case fails when run directly through
`sql-case test_crashed_foreign_key_dictionary_ddl_recovers_constraint`. That is
the next ownerless crash-recovery slice, not covered by this transaction-end
progress slice.

## Risks And Open Questions

- The exception is proven for ownerless InnoDB lock-registry and page-write
  lock-registry waiters. It does not claim SQL-level `LOCK TABLES`, global
  read-lock, or uncoordinated server-surface waits are safe.
- Broader performance work should still measure whether ownerless statement
  locks are too coarse for workloads with many blocked native row waits.
