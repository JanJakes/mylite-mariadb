# Ownerless Table-Wait Negative Proof Variants

## Problem

Ownerless lock fault coverage proves record-lock wait cleanup through SQL and
table-lock waiter cleanup through the primitive registry. SQL-level table-lock
fault injection remains unclaimed because the explored blocked SQL shapes stop
at MariaDB metadata-lock waits before InnoDB publishes a native table-lock wait
through MyLite's ownerless table-wait callback.

The existing hook SQL negative proof covers one blocked `ALTER TABLE` shape and
an initial DDL matrix. It should continue to track representative DDL variants,
including online option and existing-index mutations, so the support matrix does
not imply a stronger SQL table-wait claim than the implementation can prove.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/mdl.cc` routes ownerless metadata-lock acquisition through
  `mylite_ownerless_mdl_acquire()` and returns `ER_LOCK_WAIT_TIMEOUT` when the
  directory-backed MDL lock-table cannot grant the ticket inside the SQL lock
  wait timeout.
- `mariadb/sql/sql_base.cc` calls `open_table_get_mdl_lock()` before opening
  non-temporary tables, so many blocked DDL statements fail at MDL acquisition
  before reaching InnoDB table-lock wait publication.
- `mariadb/storage/innobase/lock/lock0lock.cc` publishes native InnoDB
  table-lock waits through `lock_table_enqueue_waiting()` and the external
  ownerless table-wait path. That is the only SQL-level hook point where the
  existing `table-lock-wait` unsafe test fault would fire.
- `packages/libmylite/src/database.cc` arms the `table-lock-wait` fault inside
  `ownerless_innodb_lock_wait_table_hook()`. If any tested SQL variant reaches
  this callback, the child process signals the test pipe and pauses.

## Design

Expand the hook-only SQL negative proof from one blocked `ALTER TABLE` to a
small matrix of representative DDL variants while one peer holds
`SELECT ... FOR UPDATE` inside an open transaction. The fixture first creates a
secondary index that the existing-index cases can attempt to drop, rename, or
mark ignored:

- `ALTER TABLE ... ADD COLUMN`
- `CREATE INDEX`
- `ALTER TABLE ... ADD INDEX ..., ALGORITHM=INPLACE, LOCK=NONE`
- `DROP INDEX` against an existing secondary index
- `ALTER TABLE ... DROP INDEX ..., ALGORITHM=NOCOPY, LOCK=NONE`
- `ALTER TABLE ... RENAME INDEX`
- `ALTER TABLE ... ALTER INDEX ... IGNORED`
- `ALTER TABLE ... FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`
- `TRUNCATE TABLE`
- `RENAME TABLE`
- `DROP TABLE`

Each variant opens a fresh ownerless SQL process with `lock_wait_timeout = 1`
and the `table-lock-wait` unsafe fault armed. Passing behavior is a MariaDB lock
wait timeout with no fault-pipe signal. A fault-pipe signal means the SQL shape
did reach the ownerless InnoDB table-wait callback, so the negative proof fails
and the paused child is killed.

The slice does not treat a broader negative proof as positive SQL table-lock
fault coverage. The compatibility docs must continue to say that SQL-level
table-lock fault injection remains planned for native table-wait paths.

## Scope And Non-Goals

In scope:

- Hook-only SQL negative-proof variants for representative blocked DDL shapes,
  including online option and existing-index metadata mutations.
- A focused hook CTest label for the table-wait SQL negative proof.
- Compatibility/spec documentation that narrows the claim to negative evidence.

Out of scope:

- Enabling ownerless `LOCK TABLES` or SQL locked-table mode.
- Claiming SQL-level table-lock fault injection coverage.
- Changing production lock acquisition or recovery behavior.
- Broader randomized DDL or external MariaDB/RQG oracles.

## Compatibility Impact

No supported SQL behavior changes. The test evidence tightens the documented
unsupported boundary: ownerless mode still rejects SQL locked-table mode, and
tested blocked DDL variants are known to stop before the ownerless native
table-wait fault hook.

## Directory And Lifecycle Impact

No directory layout changes. The focused test verifies the database remains
usable through normal ownerless reopen, forced `.shm` rebuild, and native
exclusive reopen after the blocked DDL variants time out.

## Native Storage Impact

No native storage format changes. The test explicitly avoids positive claims
about native InnoDB table-lock wait fault injection because the tested SQL
shapes do not reach the native table-wait hook.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. Test-only code and one additional CTest entry
are added under the existing unsafe ownerless hook build.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`.
- Run the focused selector:
  `./build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  table-lock-wait-negative-proof`.
- Run `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure`.
- Run the full hook ownerless SQL CTest label because the larger suite also
  includes the table-wait negative proof.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Every listed DDL variant returns a MariaDB lock wait timeout while the
  peer transaction holds the table.
- No variant signals the `table-lock-wait` fault pipe.
- The final table state can be altered, read through ownerless reopen, read
  after forced `.shm` rebuild, and read through native exclusive reopen, while
  the pre-existing secondary index remains present.
- Docs keep SQL-level table-lock fault injection marked planned rather than
  covered.

## Risks And Follow-Up

- A future MariaDB change could route one of these DDL shapes into the native
  table-wait path. That would make this negative-proof test fail, which is the
  intended signal to replace the negative proof with positive SQL fault
  injection coverage.
- Native table-lock waits may be reachable through SQL shapes not in this
  matrix. They remain a planned investigation, not an ownerless support claim.
