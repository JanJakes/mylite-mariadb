# Ownerless Generated-Column Row-Undo Crash

## Problem Statement

Ownerless native row-undo crash coverage kills full-transaction rollback and
`ROLLBACK TO SAVEPOINT` writers after one successful InnoDB `row_undo()` step.
Those tests use ordinary stored columns. Generated-column rollback remains a
separate correctness risk because base-column undo must restore stored
generated values, virtual generated expressions, and generated-column secondary
indexes without MyLite publishing partially rolled-back page images as durable
ownerless state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0undo.cc:386-415` implements
  `row_undo()`, fetches one undo record, dispatches to `row_undo_ins()` or
  `row_undo_mod()`, releases the undo page and cursor, and fires the unsafe
  `rollback-after-native-row-undo` hook after a successful native undo step.
- `mariadb/storage/innobase/row/row0umod.cc:1305-1355` implements
  `row_undo_mod()` for modified rows, including secondary-index undo before
  clustered-record undo.
- `mariadb/storage/innobase/row/row0uins.cc:550-622` implements
  `row_undo_ins()` for inserted rows, including secondary-index removal before
  clustered-record removal.
- Existing generated-column ownerless coverage proves peer refresh, generated
  expression recalculation, and generated-column secondary-index metadata/use.
  Existing native row-undo crash coverage proves one deterministic rollback
  fault boundary for ordinary columns.

## Scope And Non-Goals

In scope:

- Add hook-build crash selectors for a full transaction rollback over a table
  with stored and virtual generated columns.
- Cover generated-column secondary indexes with forced-index reads after
  recovery.
- Cover both no-live and live-peer ownerless recovery lifecycles at the
  existing `rollback-after-native-row-undo` fault.

Out of scope:

- New production recovery code unless the focused selector exposes a bug.
- Arbitrary faults inside every row-undo substep.
- Foreign-key action rollback, trigger side effects, XA/prepared rollback, DDL
  rollback, and randomized RQG-style generation.
- SQL-level local table-lock fault injection.

## Design

Create a hook-only generated-column rollback table:

```sql
CREATE TABLE app.ownerless_native_row_undo_generated (
  id INT NOT NULL PRIMARY KEY,
  base_value INT NOT NULL,
  adjust_value INT NOT NULL,
  stored_sum INT GENERATED ALWAYS AS (base_value + adjust_value) STORED,
  virtual_product INT GENERATED ALWAYS AS (base_value * adjust_value) VIRTUAL,
  payload VARBINARY(256) NOT NULL,
  INDEX ownerless_native_row_undo_generated_stored_idx (stored_sum),
  INDEX ownerless_native_row_undo_generated_virtual_idx (virtual_product)
) ENGINE=InnoDB
```

The writer updates all base columns and payloads inside one explicit
transaction, verifies the mutated generated values, arms
`rollback-after-native-row-undo` with one skipped hook hit, and executes
`ROLLBACK`. The parent kills the writer at the second successful native undo
step, after the earlier hit has been skipped.

The no-live selector verifies the next ownerless opener lets native recovery
finish rollback, waits for recovered active transactions to drain, and
preserves original base values, generated values, payloads, and forced
generated-index predicates through ownerless reopen, forced `.shm` rebuild,
ordinary native reopen, and follow-up native writes.

The live-peer selector holds another ownerless process open while the writer is
killed, requires a fresh ownerless read/write opener to return `MYLITE_BUSY`,
releases the peer, rejects shared read-only attachment until read/write
recovery runs, and then verifies the same final generated-column/index state.

## Compatibility Impact

No SQL syntax, C API, PHP API, storage format, or production behavior changes.
The slice strengthens the ownerless rollback claim for existing MariaDB
generated-column semantics.

## Directory, Lifecycle, And Native Storage Impact

No directory layout changes. All native InnoDB files, ownerless checkpoint/WAL
files, shared-memory files, and transient runtime paths remain inside the
existing MyLite database directory/runtime-root test shape.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The new
selectors are registered only in the unsafe ownerless hook build.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors:
  `transaction-rollback-generated-row-undo-crash` and
  `transaction-rollback-generated-row-undo-live-peer-crash`.
- Run the adjacent rollback hook CTest subset.
- Build the production embedded target and run adjacent generated-column and
  transaction smoke selectors to prove unsafe hooks compile out.
- Run `tools/check-ci-production-builds`, CI-style `clang-format`
  validation for touched C sources, `git diff --check`, and cleanup checks for
  ownerless processes/temp directories.

## Acceptance Criteria

- The generated-column writer reaches the existing native row-undo fault after
  skipping one earlier successful native undo step.
- No-live ownerless recovery preserves original base values, stored generated
  values, virtual generated values, payloads, and generated-column secondary
  index usability.
- Live-peer recovery remains busy until the peer exits, then recovers through
  the read/write ownerless path.
- Native DML and generic file-operation markers remain clear.
- Ownerless WAL checkpoints after recovery or is retained only as
  native-support rollback-history evidence with no page-version payload.
- Forced `.shm` rebuild and ordinary native reopen observe the same recovered
  state and accept follow-up writes.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-generated-row-undo(-live-peer)?-crash$' --output-on-failure`
  passed 2/2.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-(savepoint-rollback-(before-state|prewrite-before-state|prewrite-live-peer|native-row-undo|native-row-undo-live-peer)-crash|transaction-rollback-(before-state|native-row-undo|native-row-undo-live-peer|generated-row-undo|generated-row-undo-live-peer)-crash)$' --output-on-failure`
  passed 10/10.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
  passed, followed by production selectors
  `generated-column-index-ddl`, `generated-column-indexed-expression`, and
  `concurrent-savepoint-rollback-handoff`.
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-random-transaction-rollback-handoff$' --output-on-failure`
  passed 1/1.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed 1/1.
- Ubuntu 24.04 `clang-format --dry-run --Werror` passed for
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c`.
- `git diff --check` passed.
- `/tmp` cleanup checks found no `mylite-ownerless-*` directories.

## Risks And Follow-Up

- A follow-up retest found the original strict setup checkpoint assertion stale:
  generated-column setup can legitimately retain native-support-only rollback
  history after the forced checkpoint. The executable oracle now uses the same
  native-support-only checkpoint predicate as adjacent row-undo tests and waits
  for MariaDB recovered active transactions to drain before asserting the final
  generated-column state.
- The earliest generated-column rollback hit can drain to a stable partial
  native state with rows 1 and 2 still mutated, so the committed selector uses
  the later skip-1 boundary. Earlier generated-column row-undo substeps remain
  completion work.
- This covers generated-column side effects at one deterministic row-undo
  boundary. FK action rollback, trigger side effects, XA/prepared rollback,
  longer randomized savepoint schedules, broader redo/checkpoint
  reconciliation, and full external MariaDB/RQG stress remain completion work.
