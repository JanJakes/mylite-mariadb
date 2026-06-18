# Ownerless Append-Batch Active Fault Guard

## Problem Statement

The ownerless hook test preset compiles and enables unsafe ownerless fault
infrastructure so crash tests can arm named fault points. The visible-fast
page-log append-batch path was using that global "fault infrastructure enabled"
state as if a fault were currently armed. As a result, ordinary hook-preset
selectors and performance probes could silently fall back to direct page-log
appends even when no `MYLITE_OWNERLESS_TEST_FAULT` was configured.

That hides production-shaped append batching from the hook build and makes
timing evidence less useful. It also contradicts the focused
`single-owner-multi-row-insert-visible-fast-path` selector, which expects
multi-row visible-fast inserts to use one page-log append session.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` installs ownerless InnoDB page-publish
  hooks in `install_ownerless_innodb_lock_hooks()`. In unsafe hook builds it
  calls `mylite_ownerless_innodb_set_test_faults_enabled(1)` so named
  ownerless fault points can pause or fail when a test configures them.
- `ownerless_innodb_page_publish_batch_begin_hook()` refused to start a
  page-log append batch whenever `mylite_ownerless_innodb_test_faults_enabled_fast()`
  was nonzero, even if no fault name was configured.
- `ownerless_checkpoint_update_allows_deferred_latest_coalescing()` used the
  same broad guard, disabling deferred latest-checkpoint coalescing in hook
  builds without an active fault.
- The actual fault path in
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  consults `MYLITE_OWNERLESS_TEST_FAULT` before pausing. Global enablement is
  not the same as an armed fault.

## Design

Add a MyLite-owned helper that returns true only when unsafe hooks are compiled
and `MYLITE_OWNERLESS_TEST_FAULT` names a non-empty fault. Use that helper for:

- page-publish append-batch begin suppression,
- deferred latest-checkpoint coalescing suppression.

This keeps ordinary hook-preset tests on the production-shaped append-batch
path. Runs that configure a named fault stay conservative, preserving
individual page-publish and checkpoint crash windows.

## Compatibility Impact

There is no SQL or public API behavior change. The change affects only the
test-hook/runtime diagnostics policy:

- production builds without unsafe hooks keep the same behavior,
- hook builds without an armed fault now match production append batching,
- hook builds with `MYLITE_OWNERLESS_TEST_FAULT` remain conservative.

## Native Storage And Lifecycle Impact

Page-version WAL format, checkpoint records, redo publication, dirty-page
capture, history-proof publication, and recovery semantics are unchanged.
Append sessions still append the same records in the same ownerless database
directory; the slice only restores the intended batching policy when no fault
is active.

## Test And Verification Plan

- Rebuild the ownerless hook test target.
- Run the direct `single-owner-multi-row-insert-visible-fast-path` case and the
  focused ownerless hook selector that includes page-write locks,
  native-support WAL elision, history proof, visible-fast multi-row insert,
  native table waits, directory lifecycle, and product hooks.
- Run production stats-off and stats-enabled embedded performance probes to
  confirm append-session counters are visible in production timing.
- Run ownerless-stress or a focused ownerless stress subset.
- Run `tools/check-ci-production-builds`, format check, and `git diff --check`.

## Acceptance Criteria

- The hook visible-fast multi-row insert selector reports session appends and
  one append-session begin/end pair.
- Deferred latest-checkpoint coalescing remains visible in the same selector
  when no fault is armed.
- A named-fault run keeps batching/coalescing suppressed.
- Production probes keep publishing page versions and ownerless correctness
  counters in the expected shape.

## Risks And Unresolved Questions

This does not reduce page-version volume, history-proof publication, or native
redo/checkpoint cost. It restores production-shaped batching in hook builds and
removes misleading fallback timing from ordinary unsafe-hook selectors.

## Verification Evidence

- The direct hook case
  `mylite_ownerless_cross_process_sql_test single-owner-multi-row-insert-visible-fast-path`
  passed after the guard change.
- The focused hook selector covering page-write locks, primitives, history
  WAL proof, native-support WAL elision, multi-row visible-fast insert, native
  table waits, history-proof publish fallback, directory lifecycle, and product
  hooks passed 10/10.
- A stats-off production probe with 120 insert rows reported ownerless
  explicit-transaction inserts at `2872.19 ops/s` versus ordinary
  `4124.61 ops/s`, ownerless autocommit inserts at `1844.80 ops/s` versus
  ordinary `3777.59 ops/s`, ownerless four-row bulk rows at
  `5313.24 ops/s` versus ordinary `12074.54 ops/s`, and active-runtime
  reconnect overhead at `-0.039 ms`.
- A stats-enabled production probe showed autocommit page-log append batching
  restored: direct append calls per insert were `0.000`, session begin and end
  were each `1.000`, session append calls were `3.042` per insert, and deferred
  latest-checkpoint coalescing was `2.000` per insert. Four-row bulk kept
  `1.500` page versions per row, visible-fast commit at `1.000` per statement,
  and deferred latest-checkpoint coalescing at `8.000` per statement.
- A hook-build probe with `MYLITE_OWNERLESS_TEST_FAULT=no-such-fault` showed
  the conservative path: autocommit direct append calls were `3.625` per
  insert, session begin/append/end calls were all `0.000`, and deferred
  latest-checkpoint coalescing was `0.000`.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12 ownerless
  stress cases.
- `tools/check-ci-production-builds`, `cmake --build --preset format-check-prod`,
  and `git diff --check` passed.
