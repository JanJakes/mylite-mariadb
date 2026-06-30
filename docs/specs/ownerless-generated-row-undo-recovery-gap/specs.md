# Ownerless Generated Row-Undo Recovery Gap

## Problem Statement

The existing generated-column native row-undo crash spec claims the hook
selectors pass, but the current branch fails the no-live selector before it
reaches the crash window:

```sh
ctest --preset ownerless-test-hooks \
  -R '^libmylite\.ownerless-transaction-rollback-generated-row-undo-crash$' \
  --output-on-failure
```

The failure is the setup-only assertion that the ownerless WAL is fully
checkpointed after forcing a native checkpoint. Nearby native row-undo and
FK/trigger rollback tests now treat retained native rollback history without
page-version payload as a valid native-support-only boundary, because native
recovered transactions may still need MariaDB-owned rollback work.

Earlier local diagnosis showed that once this setup assertion is relaxed, the
generated-column selector can expose a more serious final-state mismatch: a
header-sized ownerless WAL with only part of the native generated-column update
rollback visible after recovery. This slice must separate the setup-retention
classification from the actual recovery oracle and either fix the native
rollback path or document the unsupported gap with executable evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0undo.cc:262-376` selects the latest undo
  record, advances the recovered transaction's in-memory top undo pointer to
  the previous record, and returns the undo page.
- `mariadb/storage/innobase/row/row0undo.cc:386-415` applies one undo record
  through `row_undo_ins()` or `row_undo_mod()` and fires the MyLite
  `rollback-after-native-row-undo` test hook only after a successful native row
  undo.
- `mariadb/storage/innobase/row/row0umod.cc:1176-1298` opens the affected
  table, parses update undo, searches the clustered record, and rebuilds
  indexed virtual-column values from the undo log via `row_upd_replace_vcol()`.
  Its restart comment notes that crash during recovered rollback may revisit
  already-undone records because undo-log truncation is not invoked after every
  record.
- `mariadb/storage/innobase/trx/trx0roll.cc:449-524` rolls back recovered
  active transactions in the startup background task.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:3005`
  exposes `mylite_ownerless_innodb_has_recovered_active_transactions()` for
  tests to distinguish native background rollback still in progress from a
  stable recovered state.

## Scope And Non-Goals

In scope:

- Reclassify generated-column setup checkpoint assertions so native
  support-only rollback history is accepted when no ownerless page-version
  payload remains.
- Add generated-column final-state diagnostics and a bounded wait for native
  recovered transactions to drain before asserting the recovery oracle.
- Re-run the focused generated selectors and adjacent rollback subset.
- Update stale compatibility/spec claims to match the executable result.

Out of scope:

- Broad rewrites of native InnoDB rollback internals before the focused
  generated-column failure is characterized.
- SQL-level `LOCK TABLES` fault injection, which remains unsupported and
  separately negative-proofed.
- Randomized RQG-style coverage; that depends on the bounded crash gates being
  stable first.

## Design

The generated-column test setup should use the same ownerless checkpoint
predicate as the adjacent native rollback crash tests:

- native file-operation markers must be clear,
- native DML file-operation markers must be clear,
- ownerless page-version WAL must be checkpointed, or retained only as a
  native-support proof with no page-version payload requiring ownerless replay.

The recovery oracle should wait until
`mylite_ownerless_innodb_has_recovered_active_transactions()` reports no active
recovered transactions, then assert original base, stored generated, virtual
generated, payload, and forced generated-index values. If the oracle does not
match within the existing ownerless open retry budget, the test prints a
compact aggregate state before failing.

## Compatibility Impact

No production SQL syntax, C API, storage format, or directory layout changes.
The slice corrects ownerless generated-column rollback evidence so compatibility
claims are backed by the current executable tests.

## Directory, Lifecycle, Native Storage, Build, And Size Impact

No directory-layout, lifecycle, public API, dependency, license, or production
binary-size impact is expected. Changes are test/docs only unless the focused
selector proves a production recovery bug that can be fixed narrowly.

## Test And Verification Plan

- Build the hook target:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j$(nproc)`.
- Run focused generated selectors:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-generated-row-undo(-live-peer)?-crash$' --output-on-failure`.
- Run adjacent rollback selectors after the focused result is stable.
- Run production embedded smoke selectors to prove unsafe hooks remain out of
  production builds.
- Run `tools/check-ci-production-builds`, format check, `git diff --check`, and
  cleanup checks.

## Acceptance Criteria

- Generated-column setup no longer fails solely because native-support-only
  rollback history is retained.
- The focused selector passes its original-state oracle at the first
  `rollback-after-native-row-undo` hit after native recovered transactions
  drain.
- Compatibility and slice docs no longer claim stale passing coverage.
- No ownerless test processes or `/tmp/mylite-ownerless-*` directories remain
  after verification.

## Verification Results

- The original failing command reproduced the stale setup assertion:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-generated-row-undo-crash$' --output-on-failure`.
- After the oracle fix, the focused generated selectors passed 2/2:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-generated-row-undo(-live-peer)?-crash$' --output-on-failure`.
- The earliest generated-column rollback hit reproduced a stable partial
  native state after drain (`base_sum=330`, `stored_sum=342`,
  `mutated_base_count=2`) before ownerless row-undo progress was made durable
  at the hook boundary.
- The adjacent rollback subset then exposed FK/trigger early row-undo gaps:
  update-side skip values through `8` and delete-side skip values through `4`
  could leave parent or child-side state partially applied before the same
  durability fix.
- After `row_undo()` truncates native undo progress and flushes redo in
  ownerless mode, the generated-column, FK/trigger update-side, and FK/trigger
  delete-side no-live/live-peer selectors pass at the first hook hit.

## Risks And Unresolved Questions

- A final-state mismatch with a header-sized ownerless WAL pointed toward
  native InnoDB recovered rollback progress durability, not ownerless
  page-version replay. The production fix is to make each ownerless row-undo
  step a durable native restart point before page-write hooks can expose the
  undone page image.
- Generated-column secondary indexes and virtual-column undo have more
  metadata dependencies than ordinary-row rollback; faults inside individual
  `row_undo_ins()`/`row_undo_mod()` substeps remain separate work.
