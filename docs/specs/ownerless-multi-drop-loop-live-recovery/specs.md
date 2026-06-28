# Ownerless Multi-Drop Loop Live Recovery

## Problem Statement

Ownerless multi-table `DROP TABLE` crash coverage killed the writer after
MariaDB completed the full native table list but before MyLite finished the
ownerless dictionary state. That proved final all-dropped state, but not a
crash inside MariaDB's per-table drop loop after the first table's native files
were removed and before the later table entries ran.

That window matters for cross-process ownerless concurrency because a live peer
does not restart MariaDB and therefore cannot rely on ordinary startup-only DDL
log recovery to release MyLite dictionary coordination.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` `mysql_rm_table_no_locks()` iterates
  `TABLE_LIST` entries, logs a `DDL_LOG_DROP_TABLE_ACTION`, deletes the engine
  table and `.frm`, drops triggers, and then advances to the next table.
- `mariadb/sql/ddl_log.cc` stores DROP entries in delete order and recovery
  executes only the durable entries that were written before the crash. A crash
  after the first table's entry and native deletion but before the second
  table's entry is therefore a partial-progress boundary, not an all-or-nothing
  multi-table boundary.
- `packages/libmylite/src/database.cc` already marks native file-operation DDL
  recoverable before ownerless dictionary finish. The missing boundary was the
  earlier native-progress point inside MariaDB's loop.

## Design

Add a MyLite SQL-layer dictionary hook:

- `mylite_ownerless_dictionary_set_hooks()`,
- `mylite_ownerless_dictionary_reset_hooks()`, and
- `mylite_ownerless_dictionary_native_file_op()`.

`mysql_rm_table_no_locks()` calls the hook after a non-temporary base table has
been dropped from native storage and metadata. The callback is inert unless an
ownerless runtime installed it.

When the callback fires during an active ownerless dictionary DDL, MyLite writes
the existing native file-operation checkpoint marker and marks the active
dictionary owner recoverable with the current recovery kind. The existing
prefinish path still runs for statements that complete normally. A crash before
any native progress remains unrecoverable while live peers exist, preserving the
existing begin-crash policy.

## Scope And Non-Goals

In scope:

- Same-schema two-table `DROP TABLE a, b` killed after the first table's native
  file-operation boundary.
- Live-peer recovery of the partial native state.
- Native file-operation marker retention while a peer remains live and no-live
  drain after release.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Cross-schema in-loop multi-drop, mixed views, triggers, temporary tables, and
  `DROP DATABASE` internal table-list loops.
- Completing remaining unexecuted DROP entries in an already-live peer.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL syntax or public behavior is newly enabled. The slice aligns ownerless
live recovery with MariaDB's durable DDL-log progress boundary for a crash
inside `DROP TABLE` processing: entries completed before the crash remain
applied, and entries not yet logged remain present.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or durable format changes. The hook writes existing MyLite
ownerless checkpoint and dictionary-state evidence, then relies on existing
native checkpoint drain and `.shm` rebuild behavior.

## Public API, Build, Size, License

No public API, dependency, or license changes. The MariaDB embedded archive gains
one small MyLite-owned SQL hook source file and one narrow upstream-derived call
site in `sql_table.cc`.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector
  `dictionary-multi-drop-loop-crash`.
- Run the adjacent multi-drop hook CTest group.
- Run production formatting and CI-production-build guards.
- Rebuild the MariaDB embedded archive if the freshness guard requires it.
- Run `git diff --check`.

## Acceptance Criteria

- The killed writer stops after the first native table drop and before ownerless
  dictionary finish.
- A live peer recovers the dictionary state while the native file-operation
  marker remains set.
- The first table is absent; the second table remains present, readable, and
  writable.
- Final no-live recovery clears the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the same partial-progress state.

## Risks And Follow-Up

- This proves a same-schema in-loop boundary only. Cross-schema and
  schema-drop-internal loops remain planned.
- The hook does not replay unlogged future DDL entries in an already-live peer;
  it exposes MariaDB's durable progress at the crash point.
- Broader DDL/file-lifecycle recovery and external randomized DDL stress remain
  planned.
