# Ownerless Prepared DML Native Lifetime

## Problem

The history-proof attribution slice exposed an ownerless checksum-stress
readiness failure around prepared DML. The default checksum stress forks four
writer processes and one reader; two writers prepare an InnoDB `UPDATE` before
they signal ready. Reduced reruns showed:

- all-direct writers passed repeatedly;
- one prepared writer could make a direct writer enter native execution and
  hold the ownerless table-write statement lock until peers timed out;
- two prepared writers reproduced the original readiness/stress shape.

Serializing `mysql_stmt_prepare()` with an additional MyLite byte-range lock
did not fix the failure. Closing the native `MYSQL_STMT` immediately after
public `mylite_prepare()` also was not sufficient. The evidence points to the
native MariaDB/InnoDB prepare path leaving process-local table or dictionary
state live inside the preparing process even after the client statement handle
is closed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc` `Prepared_statement::prepare()` parses and
  context-checks the statement, records an MDL savepoint, runs
  `check_prepared_statement()`, calls `close_thread_tables_for_query()`, and
  rolls MDL back to the prepare-time savepoint. That proves MariaDB releases
  prepare-time SQL metadata locks, but it does not prove every InnoDB
  dictionary/table-cache side effect is absent from the process.
- `mariadb/sql/sql_prepare.cc` `Prepared_statement::execute()` re-enters the
  normal `mysql_execute_command()` path and calls `cleanup_stmt()` after
  execution when no cursor remains. Ownerless DML should therefore place native
  prepare and execute inside the same MyLite statement boundary when the public
  prepared statement is a write.
- `mariadb/storage/innobase/dict/dict0dict.cc` defines process-global
  `dict_sys` and its fatal `dict_sys.latch` wait message. This reinforces that
  InnoDB dictionary state is native process-local state, not directory-owned
  ownerless state.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_checksum_stress()` prepares DML before child readiness for
  prepared workers. This is the focused reproducer for native prepared DML
  lifetime across independent ownerless processes.

## Design

For ownerless read/write handles, ordinary no-result write prepared statements
defer native MariaDB statement creation:

- public `mylite_prepare()` still rejects unsupported MyLite SQL policy;
- for `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` without `RETURNING`, MyLite
  counts `?` parameter markers with its SQL tokenizer and stores the SQL text
  plus ownerless policy tokens;
- no `MYSQL_STMT` is created at public prepare time for that subset;
- each first `mylite_step()` after reset creates a native `MYSQL_STMT`,
  prepares it, verifies the parameter count and no-result shape, binds current
  parameters, executes under the existing ownerless statement/page/dictionary
  boundary, reads affected-row and insert-id metadata, and closes the native
  statement before returning;
- result-returning statements, prepared reads, and unsupported/complex write
  shapes keep the existing native prepare path.

This keeps native prepared DML table-open and dictionary side effects scoped to
the same protected execution interval as ordinary ownerless text DML. It does
not add a new directory file, lock byte, background process, or durable format.

## Compatibility Impact

The public prepared-statement object and binding API remain available. Parameter
count for this ownerless DML subset is now derived from MyLite's SQL tokenizer
instead of from MariaDB's prepare response. Syntax, table, column, privilege,
and storage-engine validation for that subset can be reported by
`mylite_step()` instead of `mylite_prepare()`.

That validation-timing difference is ownerless-read/write specific and bounded
to ordinary no-result `INSERT`, `UPDATE`, `DELETE`, and `REPLACE`. The tradeoff
is intentional: keeping native MariaDB prepared DML state alive between public
MyLite calls is not currently safe across independent ownerless processes.

## Database Directory And Lifecycle Impact

No durable directory-layout changes are introduced. The change reduces native
process-local state that can survive across ownerless readiness or wait
barriers. Existing ownerless statement locks, page-version refresh, dictionary
DDL, and page-write publication remain the execution boundary.

## Native Storage Impact

Native InnoDB DML still executes through MariaDB prepared statements. The native
prepared handle is now per-step for the affected ownerless DML subset, so native
table/dictionary side effects are created and destroyed while MyLite holds the
same ownerless statement boundary that protects ordinary DML execution.

## Build, Size, And Dependency Impact

No new dependencies or storage formats. The code adds a small MyLite-owned
native-statement lifetime path and reuses the existing SQL tokenizer.

## Test And Verification Plan

- Build the focused ownerless stress target:
  `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`.
- Run reduced checksum-stress controls:
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=1
  MYLITE_OWNERLESS_CHECKSUM_STRESS_PREPARED_WRITERS=0|1|2 timeout 120s
  build/ownerless-stress/packages/libmylite/mylite_ownerless_cross_process_sql_test
  checksum-stress`.
- Run the registered checksum stress:
  `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-checksum-stress$'
  --output-on-failure`.
- Run prepared read/write focused embedded selectors and nearby ownerless stress
  coverage for checksum, child-failure cleanup, active-reader pressure, and FK
  graph behavior. Random transaction stress remains a separate correctness
  gap if it reproduces an expected-total mismatch where ownerless and native
  final reads agree with each other.
- Run format and diff whitespace checks.

## Acceptance Criteria

- All-direct, one-prepared, and two-prepared one-round checksum-stress controls
  pass repeatedly.
- The registered checksum stress no longer stalls before child readiness on
  prepared DML setup.
- Ordinary ownerless prepared DML still reports correct parameter counts,
  executes with bound values, reports affected rows, supports reset/re-execute,
  and finalizes cleanly.
- No new global owner, daemon, durable file, or broad ownerless prepare lock is
  introduced.

## Risks And Unresolved Questions

- Ownerless no-result write prepared statements now move some validation from
  `mylite_prepare()` to `mylite_step()`. Broader drop-in coverage should decide
  later whether MyLite can recover prepare-time diagnostics without creating
  unsafe native InnoDB state.
- Result-returning DML and complex prepared write shapes remain on the native
  prepare path until separate evidence proves they need and can use the same
  lifetime boundary.
- Per-step native prepare has a performance cost for prepared DML. Correctness
  takes priority here; a later optimization can cache only when a directory-
  owned proof shows the native state is safe across live ownerless peers.
