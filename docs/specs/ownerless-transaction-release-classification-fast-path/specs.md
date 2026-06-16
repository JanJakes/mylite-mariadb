# Ownerless Transaction-Release Classification Fast Path

## Problem

Ownerless autocommit writes still spend visible time in the native InnoDB
mini-transaction page-write path after previous slices separated PHP startup,
WordPress setup, page-log append work, checkpoint sync, page-publish buffer
allocation, and page-write publish subphases.

The stats-enabled production probe now shows the remaining native page-write
cost split across page-version hook/page-log append, commit-log publish,
redo-leave, and no-dirty commit-loop work. One small ownerless-specific cost
inside that path is repeated transaction-release classification. For
statement-visible autocommit writes, transaction-level page release is not
active, but some loops still recompute the helper or classify modified pages as
potential transaction-deferred pages.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_uses_transaction_release()`
  checks ownerless hook state, startup/recovery state, current transaction
  state, lock-only transaction shape, read-only/dictionary flags, autocommit
  state, visible-fast-path eligibility, transaction id state, and `THD`/LEX
  state.
- `ownerless_page_write_holds_for_transaction()` delegates to the page
  deferral classifier, which may identify undo tablespaces through native
  tablespace state.
- `ownerless_page_write_enter()` called those helpers multiple times for the
  same page and mini-transaction path.
- `ownerless_page_writes_publish()` recomputed transaction-release state inside
  the memo scan and classified each modified page before checking whether
  transaction-release publication was active.
- The no-dirty commit-log release path also rechecked transaction-release state
  inside its modified-page publish branch.

## Design

Cache transaction-release classification at mini-transaction scope where the
decision is already stable:

- `ownerless_page_write_enter()` computes `uses_transaction_release` and
  `holds_for_transaction` once after the lock-only early exits and reuses the
  result through wait handling, already-modified checks, transaction page
  tracking, refresh decisions, and boundary publication decisions.
- `ownerless_page_write_leave()` computes `holds_for_transaction` only when
  transaction release is active and passes that decision into
  `ownerless_page_write_release_deferred()`, so the deferred-release helper no
  longer repeats the same classification.
- `ownerless_page_writes_publish()` computes transaction-release state once
  before scanning modified memo slots and only calls the per-page transaction
  publish classifier when transaction release is active.
- The no-dirty commit-log branch computes transaction-release state once before
  releasing memo slots and reuses it for modified-page publish decisions.
- `release_unlogged()` computes ownerless hook state and transaction-release
  state once before its memo release loop.

No page locks, lock releases, refreshes, boundary publications, transaction
captures, page-version records, history-proof records, redo calls, or
checkpoint calls are removed.

## Compatibility Impact

This is an internal ownerless write-path performance change. SQL behavior,
public APIs, native storage formats, WAL/checkpoint record formats, page
visibility, lock ordering, and unsupported-surface policy are unchanged.

## Performance Impact

The change avoids repeated helper calls and page deferral classification on the
hot path, especially for statement-visible autocommit writes where
transaction-level release is inactive. It does not change the remaining
history-proof page publication volume or the broader redo/checkpoint recovery
work.

## Test Plan

- Rebuild the production embedded performance probe.
- Run focused ownerless single-owner/native-support SQL coverage to exercise
  history proof, native-support page elision, and transaction-visible commit
  behavior.
- Run a reduced stats-enabled production embedded performance probe and compare
  the page-write publish/commit-log summary rows with the pre-change sample.
- Run production-build guards, format check, and whitespace checks.

## Verification Notes

Local production smoke after the code change rebuilt the MariaDB embedded
archive and the `php-embedded-prod` probe/harness targets. Focused
`ownerless-single-owner-history-wal-proof` and
`ownerless-single-owner-native-support-page-wal-elision` selectors passed. A
reduced stats-enabled 1000-row attribution sample kept the ownerless/ordinary
autocommit ratio effectively unchanged from the preceding local sample
(`0.3693` versus `0.3703`), while a stats-off 2000-row throughput smoke
reported ownerless autocommit at `1330.26 ops/s`, ordinary autocommit at
`3588.25 ops/s`, ratio `0.3707`. After the final source cleanup and rebuild, a
1000-row stats-off smoke reported ownerless autocommit at `1340.74 ops/s`,
ordinary autocommit at `3669.94 ops/s`, ratio `0.3653`. Treat this slice as a
cleanup of redundant hot-path classification, not as the larger
page-publication or redo/checkpoint performance fix.

## Acceptance Criteria

- Transaction-release and per-page transaction-deferral classification are not
  recomputed inside the hot ownerless page-write loops when the mini-transaction
  decision is already known.
- Ownerless page-write lock release, deferred release, page publication,
  boundary publication, and history-proof behavior remain unchanged.
- Focused ownerless correctness checks and production performance smoke checks
  pass.

## Risks And Non-Goals

- This is a micro-optimization. It should reduce repeated classification work,
  but it does not solve the larger page-log append, history-proof publication,
  or redo/checkpoint reconciliation targets by itself.
- The cached decisions are intentionally scoped to one mini-transaction call
  path. Broader transaction state caching would need separate proof because
  transaction shape can change across SQL statements and recovery phases.
