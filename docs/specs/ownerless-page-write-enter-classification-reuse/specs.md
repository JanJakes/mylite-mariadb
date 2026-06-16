# Ownerless Page-Write Enter Classification Reuse

## Problem

Recent production ownerless performance probes show the remaining simple
autocommit write cost is concentrated in native InnoDB page-write publication,
history-proof page-version append work, and commit-log subphases. The previous
transaction-release classification fast path removed several repeated helper
calls, but the modified-page entry sites still call
`ownerless_page_write_enter()` and then immediately recompute the same
transaction-release/page-deferral predicate before marking the page for
transaction-deferred dirty publishing.

This slice reuses the already-computed write-enter classification. It is a
bounded hot-path cleanup, not the broader native redo/checkpoint or
history-proof representation change that remains the larger throughput target.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_enter()`
  already computes `uses_transaction_release` and
  `holds_for_transaction` after non-ownerless, non-persistent, plain-read, and
  lock-only exits.
- `mtr_t::set_modified()`, `mtr_t::init()`, and the inline
  `mtr_t::memo_push()` modify path in
  `mariadb/storage/innobase/include/mtr0mtr.h` call
  `ownerless_page_write_enter()` and then repeat
  `ownerless_page_write_uses_transaction_release()` before calling
  `ownerless_page_write_note_dirty_transaction_page()`.
- `mtr_t::ownerless_page_writes_publish()` and
  `mtr_t::commit_log()` compute a local `transaction_publish` predicate but
  call the dirty-page helper that rechecks the same page-deferral predicate.
- `ownerless_page_write_publishes_with_transaction()` and
  `ownerless_page_write_holds_for_transaction()` both delegate to the same
  transaction deferral classifier today, including undo-tablespace detection.

## Design

Change `ownerless_page_write_enter()` to return whether the current page is
held for transaction-release publishing. All post-classification exits return
the computed `holds_for_transaction` value so duplicate MTR pages, wait/retry
paths, already-transaction-dirty pages, lock timeout/deadlock paths, and
refresh/boundary paths preserve the previous downstream dirty-page decision.

The early exits that previously made `ownerless_page_write_uses_transaction_release()`
false continue returning `false`. The SQL plain-read/select exit returns the
old combined downstream predicate so any unusual persistent modification that
previously reached dirty-page tracking through the caller keeps doing so.

Add an overload of `ownerless_page_write_note_dirty_transaction_page()` that
accepts an already-computed transaction-held-page predicate. Use it at call
sites that already proved the page will publish with transaction release:

- inline modified-page `memo_push()`;
- `mtr_t::set_modified()` when the page is not already modified in the memo;
- `mtr_t::init()`;
- `ownerless_page_writes_publish()` after `transaction_publish`;
- `commit_log()` after the no-dirty commit publish predicate.

When `set_modified()` sees that the page is already modified in the memo and
therefore intentionally skips `ownerless_page_write_enter()`, it computes the
old predicate directly.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, storage-engine file format,
WAL/checkpoint format, directory layout, or unsupported-surface behavior
changes. The slice only removes redundant internal ownerless classification
work on existing page-write paths.

## Directory And Lifecycle Impact

No durable files, shared-memory records, page-log records, checkpoint records,
native tablespaces, temporary directories, or cleanup rules change.

## Native Storage Impact

Native InnoDB page latching, redo, undo, flush-list insertion, page-version
publication, history-proof publication, boundary publication, transaction
dirty-page capture, and ownerless lock release behavior remain unchanged. The
existing native page classes that require transaction-deferred publishing still
do so.

## Build And Performance Impact

The slice changes upstream-derived InnoDB mini-transaction code, so the
MariaDB embedded archive must be rebuilt before production verification. The
expected performance effect is small: fewer repeated ownerless helper calls
and fewer repeated page-deferral classifications at modified-page entry and
commit-publication sites. It does not reduce the remaining page-version record
count, native-support history-proof page count, redo-leave work, or checkpoint
work.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with the production baseline.
- Build production `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run focused ownerless SQL selectors for transaction history proof, native
  support elision, active-reader/refresh behavior, and cross-process commit
  visibility.
- Run a reduced production stats-enabled performance probe and verify page
  publication counts, history-proof counts, and publish failure counters stay
  in the expected range.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Modified-page entry sites reuse `ownerless_page_write_enter()` classification
  instead of immediately recomputing transaction-release state.
- Commit-publication sites that already computed `transaction_publish` do not
  reclassify the same page in the dirty-page helper.
- Focused ownerless correctness checks continue passing with production
  artifacts.
- Performance documentation records this as a micro-optimization, not
  completion of the ownerless write-throughput work.

## Verification Results

Local verification on 2026-06-16 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `tools/mariadb-embedded-build build` rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- Direct focused production selectors passed for
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`,
  `active-pin-reclaim-boundary`, `prepared-committed-read`, and
  `commit-race`.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision)|active-pin-reclaim-boundary|prepared-committed-read|commit-race)$'
  --output-on-failure` passed the two registered single-owner tests matched by
  that production preset.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=1000`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. It preserved the
  expected publication shape: `2.004` native-support published pages per
  insert, `1.000` history-proof rollback-segment page per insert, `1.000`
  history-proof undo page per insert, `4.000` checkpoint updates per insert,
  and `4.000` legacy checkpoint writes elided per insert. The short
  stats-enabled timing sample was noisier and slower than the preceding local
  sample (`0.3384` ownerless/ordinary autocommit ratio versus `0.4619`), so it
  is correctness/count evidence rather than a throughput claim.
- A reduced stats-off production smoke with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=50`,
  and `MYLITE_PERF_INSERT_ITERATIONS=2000` reported ownerless autocommit at
  `1476.40 ops/s`, ordinary autocommit at `3383.42 ops/s`, and ratio `0.4364`.
  That is in the same local range as the preceding sample (`1552.97 ops/s`
  ownerless, `3361.88 ops/s` ordinary, ratio `0.4619`) and does not show a
  large stats-off production-path regression.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`, `cmake --build --preset format-check-prod`, and
  `git diff --check` passed.

## Risks And Follow-Up

- The change is intentionally conservative and should only move a small part
  of the ownerless write-path profile.
- The larger remaining work is still replacing or compressing the
  history-proof/native-support page publication while preserving recovery,
  checkpoint, and active-reader evidence.
