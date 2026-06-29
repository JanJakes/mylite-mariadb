# Ownerless Active-Reader Pressure Dead-Writer Cleanup

## Problem

Ownerless active-reader pressure already covers retained WAL while a
repeatable-read reader pin is live, a killed reader pin, diagnostics, and broad
DML/DDL pressure policy classes. The remaining crash-oriented pressure gap is a
writer process that reaches the pressure gate, receives `MYLITE_BUSY`, and then
dies without closing its database handle or prepared statements while the
reader pin and another live peer remain.

This slice adds deterministic cleanup evidence for that dead pressure writer
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/read/read0read.cc` documents that active read
  views determine what purge may remove, and
  `ReadView::open()`/`ReadView::close()` publish and release ownerless read
  view state.
- `mariadb/storage/innobase/trx/trx0trx.cc` drives InnoDB purge state from
  active read views and transaction history. Ownerless pressure must not allow
  retained page-version WAL to be reclaimed while a peer read view can still
  need older images.
- `packages/libmylite/src/database.cc`:
  `enforce_ownerless_page_log_limit_policy()` runs before ownerless runtime
  statement setup and before MariaDB execution for both text SQL and prepared
  statement steps. If retained WAL is at the configured limit and an active
  page-version pin exists, the statement returns `MYLITE_BUSY` with no MariaDB
  errno.
- `mariadb/sql/sql_parse.cc` marks view and trigger DDL as
  `CF_CHANGES_DATA`, and routes `CREATE OR REPLACE VIEW`/`ALTER VIEW` through
  `mysql_create_view()` and trigger DDL through
  `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_table.cc` and `mariadb/storage/innobase/row/row0mysql.cc`
  route `CREATE OR REPLACE TABLE ... LIKE`, FK ADD, and FK DROP through native
  table and InnoDB dictionary metadata paths that must not be reached by a
  pressure-rejected writer.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already
  contains active-reader pressure, killed-pin, write-policy, and diagnostics
  selectors. The new selector can reuse those helpers and add process death
  after pressure rejection.

## Scope And Non-Goals

In scope:

- Hold one idle live ownerless peer and one repeatable-read snapshot reader.
- Commit an ownerless update that leaves page-version WAL retained by the
  reader pin.
- Start a pressure-limited writer that receives `MYLITE_BUSY` for direct DML,
  representative table DDL, representative rename DDL, FK ADD/DROP,
  replacement-copy table DDL, view replacement, trigger replacement, and a
  prepared DML step, then kill it without `mylite_close()` or statement
  finalization.
- Verify a later writer cleans the dead process state, still sees pressure
  while the reader pin is live, and observes no DML or DDL side effects from
  the killed writer.
- Verify DML and the blocked DDL succeed after the reader releases while the
  idle peer remains live, then survive ownerless/native reopen and forced
  `.shm` rebuild.

Out of scope:

- New pressure policy classes beyond the existing broad write-policy selector.
- New production pressure throttling behavior.
- SQL-level local table-lock fault injection.
- External MariaDB/RQG randomized active-reader pressure stress.

## Design

Add a focused ownerless SQL selector:
`active-reader-pressure-dead-writer`.

The selector builds a small DML/DDL fixture, starts an idle ownerless peer,
starts a repeatable-read reader pin, and commits one ownerless update to create
retained page-version WAL. It then opens a child writer with
`ownerless_page_log_limit_bytes` set to the retained WAL size. The child proves
the pressure gate rejects:

- direct `UPDATE`,
- `ALTER TABLE ... ADD COLUMN`,
- `RENAME TABLE`,
- `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY`,
- `ALTER TABLE ... DROP FOREIGN KEY`,
- `CREATE OR REPLACE TABLE ... LIKE`,
- `CREATE OR REPLACE VIEW`,
- `CREATE OR REPLACE TRIGGER`, and
- prepared `INSERT ... SELECT` at `mylite_step()`.

The child then waits with its database handle and prepared statement still
open. The parent kills it with `SIGKILL`, opens another pressure-limited writer
to trigger stale process cleanup, and verifies pressure remains active because
the real reader pin is still live. After the reader releases, the same writer
can update rows and execute the DDL while the idle peer is still live, including
FK enforcement/release, replacement-copy metadata, view projection, and trigger
body effects. Final ownerless and native reopens verify durable state after
`.shm` rebuild.

## Compatibility Impact

No SQL semantics change. The slice adds evidence that pressure-limit
`MYLITE_BUSY` is a pre-execution MyLite throttle and that killing a throttled
writer does not create durable SQL side effects, stale pressure pins, or stale
process state that blocks later writers after the real reader pin is gone.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing process registry
cleanup, read-view registry cleanup, page-version WAL retention, checkpointing,
and forced shared-memory rebuild inside the MyLite database directory.

## Native Storage Impact

No native storage format changes. The killed writer must not reach MariaDB
native DML/DDL mutation paths for the pressure-rejected statements. After
pressure clears, MariaDB native InnoDB DML and DDL execute normally and remain
durable across ownerless and native reopen.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds tests and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run the focused selector in the hook build:
  `active-reader-pressure-dead-writer`.
- Run the adjacent active-reader pressure CTest subset in the hook build.
- Build `mylite_ownerless_cross_process_sql_test` in `embedded-prod`.
- Run the focused selector in the production embedded build.
- Run the CI production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- A pressure-limited writer killed after pressure-rejected DML, representative
  table/rename DDL, FK ADD/DROP, replacement-copy, view, trigger, and prepared
  DML leaves no SQL side effects.
- A later writer cleans the dead writer slot but remains pressure-throttled
  while the original reader pin is live.
- After the reader releases, DML and the representative DDL families succeed
  even while another idle ownerless peer remains live.
- Final state survives ownerless reopen, native read/write reopen, and forced
  `.shm` rebuild.

## Risks And Follow-Up

- This is deterministic process-death coverage, not a randomized external
  pressure oracle.
- Broader active-reader pressure crash matrices for every write-policy class,
  longer external MariaDB/RQG runs, and unrelated DDL/file-lifecycle recovery
  remain separate completion gates.
