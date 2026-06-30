# Ownerless Native Reclaim Crash Threshold

## Problem

The unsafe-hook selectors `native-reclaim-crash` and `native-reclaim-race`
are intended to pause or kill an ownerless writer after native checkpoint proof
but before page-log reclamation. After the single-owner foreground reclaim
budget slice, those selectors no longer reached the
`native-checkpoint-before-reclaim` fault when their writer performed only a
small single-row update. The production behavior is correct: a still-single-
owner writer can retain small page-version WAL and avoid foreground reclaim
work. The deterministic crash tests must instead generate enough page-version
WAL to exercise close-time native checkpoint reclamation.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`:
  `maybe_reclaim_ownerless_page_log_after_statement()` attempts foreground
  reclaim only after `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES` and, for
  a still-single-owner runtime without a native file-operation marker, defers
  foreground reclaim until the larger
  `MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES` budget.
- `packages/libmylite/src/database.cc`:
  `release_runtime()` still calls
  `reclaim_ownerless_page_log_after_native_checkpoint()` during final close.
  That path is the product correctness boundary for native checkpoint proof,
  page-version WAL compaction, and page-index replacement.
- `packages/libmylite/src/database.cc`:
  `prepare_ownerless_page_log_native_checkpoint_for_reclaim()` contains the
  unsafe `native-checkpoint-before-reclaim` fault after native checkpoint
  coverage and before reclamation.
- `docs/specs/ownerless-single-owner-foreground-reclaim-budget/specs.md`
  explicitly keeps timer and close-time reclaim on the normal threshold while
  raising only single-owner foreground statement reclaim.

## Design

Keep the production reclaim scheduling unchanged. Repair the crash selector by
making its child writer hold a prepared result cursor while it updates a
pre-created InnoDB payload table with enough user-page data to produce
retained page-version WAL and updates the original small ownerless table. The
active cursor suppresses statement-boundary cleanup. The child then finalizes
the cursor, installs the fault, and closes the final writer handle so the
pause remains in the native checkpoint proof path used by final close.

Repair the race selector with a statement-boundary variant of the same native
reclaim proof. Its child writer first observes a short-lived peer ownerless
open, which disables the single-owner foreground reclaim budget for that
runtime. The threshold-crossing update can then pause at
`native-checkpoint-before-reclaim` before shutdown serialization begins,
allowing the parent process to open a peer writer and commit the newer update
that the race is meant to prove. The race writer wraps the original small-row
update and the payload-table update in one explicit transaction so the fault
fires only after both updates commit.

The parent creates and checkpoints the payload table before forking so the
crash selectors continue to prove DML recovery around the native reclaim
boundary rather than conflating the test with DDL file-lifecycle recovery.

## Compatibility Impact

No SQL semantics, public API, directory layout, or native storage format
change. The slice restores deterministic test evidence for the existing
native checkpoint reclamation claim while preserving the performance-oriented
single-owner foreground budget.

## Directory And Lifecycle Impact

All data remains inside the MyLite database directory. The test uses a normal
file-per-table InnoDB table under `datadir/` and the existing
`concurrency/mylite-concurrency.wal` and `.ckpt` recovery anchors.

## Native Storage Impact

Native InnoDB checkpointing and WAL reclamation code is unchanged. The test
workload simply ensures the close path reaches the already-designed native
checkpoint proof boundary.

## Test And Verification Plan

- Run direct unsafe-hook selectors:
  `native-reclaim`, `native-reclaim-crash`, and `native-reclaim-race`.
- Run the hook `crash-tail` aggregate far enough to prove these selectors no
  longer fail before later crash cases.
- Run focused ownerless CTests for hook-enabled embedded SQL and primitives.
- Run production build configuration and formatting checks.
- Run `git diff --check`.

## Acceptance Criteria

- `native-reclaim-crash` keeps statement-boundary reclaim suppressed until
  final close, pauses at `native-checkpoint-before-reclaim`, can be killed,
  and ownerless plus native reopen preserve both the original small row update
  and the larger payload-table update while allowing rollback-history
  native-support-only WAL retention after user-page records drain.
- `native-reclaim-race` pauses at the same native checkpoint proof boundary
  from statement-boundary reclaim, a peer can commit a newer update, the
  paused writer can resume without growing or invalidating already reclaimed
  user-page WAL, may either strictly shrink the retained WAL or finish
  checkpointing it, and ownerless plus native reopen preserve both commits while
  allowing rollback-history native-support-only WAL retention.
- The ordinary `native-reclaim` selector still passes.
- The fix does not lower reclaim budgets or add new production hooks.

## Verification Results

Completed on 2026-06-13:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Direct selectors passed: `native-reclaim`, `native-reclaim-crash`, and
  `native-reclaim-race`.
- `active-pin-reclaim-boundary` and `consistent-snapshot-pin-race` passed
  after the adjacent consistent-snapshot live-pin fix.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$'
  --output-on-failure` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- This slice repairs deterministic native reclaim crash evidence. It does not
  close broader redo/checkpoint reconciliation, active-reader pressure policy,
  or DDL/file-lifecycle crash matrices.
- The full hook `crash-tail` aggregate exposed a later
  `record-lock-grant-crash` statement-gate inversion after the native-reclaim
  and active-pin cases. That follow-up is tracked by
  `ownerless-transaction-end-lock-grant-progress`, where transaction end can
  proceed when the shared registries prove it owns the native/page-write lock
  blocking a peer waiter.
