# Ownerless MTR Publish List

## Problem

The ownerless made-dirty mini-transaction commit path walks the MTR memo once
to install the commit LSN and add modified pages to the InnoDB flush list, then
walks the same memo again in `ownerless_page_writes_publish()` to find the
modified pages that need MyLite page-version publication.

A reduced production attribution sample after page-log checksum handoff showed
`mylite_perf_summary_ownerless_autocommit_page_write_publish_scan_ms_per_insert=0.049`
and `mylite_perf_summary_ownerless_autocommit_page_write_publish_scan_calls_per_insert=1.275`.
That scan is smaller than the remaining native commit costs, but it is a
bounded redundancy in a hot ownerless write path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` handles the
  `m_made_dirty` branch by scanning `mtr->m_memo` in reverse while holding
  `buf_pool.flush_list_mutex`, writing each modified page's `FIL_PAGE_LSN`,
  mirroring compressed-page LSN state, and adding the block to the flush list.
- The same `m_made_dirty` branch releases the commit-log latch, leaves
  ownerless redo serialization, then calls
  `mtr_t::ownerless_page_writes_publish()` before `mtr->release()`.
- `mtr_t::ownerless_page_writes_publish()` starts a page-log append batch,
  recomputes whether the MTR uses transaction release, walks `m_memo` again,
  and publishes each `MTR_MEMO_MODIFY` page or captures it for transaction
  release.
- The no-dirty commit branch already publishes modified pages inline while it
  releases memo slots, because it is already walking the memo for release.
- Previous MTR wrapper experiments that cached transaction-release decisions
  or reused tracked-page membership for release were rejected after stress
  failures. This slice does not retry those decisions.

## Design

Collect modified `buf_page_t` pointers during the existing `m_made_dirty`
flush-list scan when ownerless hooks are enabled. After `commit_log_release()`
and `ownerless_redo_leave()`, publish the collected pages through a new helper
that preserves `ownerless_page_writes_publish()` behavior over an explicit page
list:

- begin the same MyLite page-publish batch,
- compute `ownerless_page_write_uses_transaction_release()` at publish time,
- for each collected page, either mark/capture transaction-deferred pages or
  publish the page image immediately, and
- end the page-publish batch.

The collected pages are still latched and fixed until `mtr->release()`, so the
page image lifetime matches the existing second-scan path. Publication still
happens after the redo/log latch release and before memo release.

The existing `ownerless_page_writes_publish()` helper remains available for
callers that have not already collected modified pages.

## Scope And Non-Goals

In scope:

- `m_made_dirty` ownerless MTR commit publication in
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`,
- performance probe evidence that publish scan calls/time fall on the
  ownerless autocommit sample, and
- docs/compatibility updates describing the bounded hot-path pruning.

Out of scope:

- changing page-version WAL format or recovery,
- eliding native-support/history-proof page publication,
- caching ownerless transaction-release decisions across MTR phases,
- changing ownerless page-write lock release behavior,
- changing no-dirty branch memo release behavior, and
- broader group-commit or page-visible checkpoint batching.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, or durable file-format change. The same
committed page images are published at the same commit LSN; only the way the
made-dirty path finds those pages changes.

## Directory And Lifecycle Impact

No file, directory, runtime-open, or shutdown lifecycle change.

## Native Storage Impact

Native InnoDB redo, flush-list insertion, page LSN installation, page latch
release, and history-proof publication ordering remain unchanged. The
collected page pointers are used only before the MTR releases its memo slots.

## Build, Size, License, And Dependencies

No dependency or license change. Binary-size impact is limited to a small
helper and a small ownerless-only page-pointer vector in the made-dirty commit
path.

## Test And Verification Plan

- Build the production PHP embedded ownerless SQL harness and performance
  probe.
- Run focused ownerless single-owner publish-path selectors.
- Run a reduced stats-enabled production performance probe and verify
  ownerless autocommit publish scan calls/time fall while publish/page-version
  counts remain sane.
- Run hook-build visible publish/checkpoint crash cases.
- Run ownerless stress focused selectors.
- Run production build guards, format, and whitespace checks.

## Verification Results

Local verification on 2026-06-16 used production embedded builds:

- `tools/mariadb-embedded-build build` passed and rebuilt the MariaDB embedded
  archive with `CMAKE_BUILD_TYPE=MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- Focused production ownerless SQL selectors passed:
  `ownerless-single-owner-history-wal-proof`,
  `ownerless-single-owner-native-support-page-wal-elision`, and
  `ownerless-single-owner-multi-row-insert-visible-fast-path`.
- A reduced stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported
  `mylite_perf_summary_ownerless_autocommit_page_write_publish_scan_calls_per_insert=0.000`,
  `mylite_perf_summary_ownerless_autocommit_page_write_publish_scan_ms_per_insert=0.000`,
  `mylite_perf_summary_ownerless_autocommit_mtr_published_pages_per_insert=3.000`,
  `mylite_perf_summary_ownerless_autocommit_page_log_append_calls_per_insert=3.020`,
  ordinary autocommit at `3419.75 ops/s`, ownerless autocommit at
  `1471.79 ops/s`, and an ownerless/ordinary ratio of `0.4304`. The short
  200-row throughput sample is volatile and is not the acceptance criterion
  for this slice; the bounded attribution target is the eliminated redundant
  MTR publish scan while preserving page-version counts.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and direct hook selectors
  `visible-publish-crash`, `page-publish-before-append-crash`,
  `visible-checkpoint-crash`, `redo-latest-crash`, and
  `redo-latest-checkpoint-crash` passed.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test`
  passed, and the focused ownerless-stress selectors passed:
  `ownerless-primitives`, `ownerless-single-owner-history-wal-proof`,
  `ownerless-single-owner-native-support-page-wal-elision`, and
  `ownerless-single-owner-multi-row-insert-visible-fast-path`.
- Production build guards passed for `build/mariadb-embedded`,
  `build/php-embedded-prod`, `build/ownerless-test-hooks`, and
  `build/ownerless-stress`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Made-dirty ownerless MTR publication no longer rescans the full MTR memo just
  to rediscover modified pages.
- Page-version publish counts and ownerless committed-read behavior remain
  correct.
- The reduced production performance probe shows lower
  `page_write_publish_scan_*` attribution for the ownerless autocommit sample.
- Hook crash and focused stress coverage still pass.

## Risks And Follow-Up

- The optimization assumes modified page pointers collected during the
  flush-list pass remain valid until `mtr->release()`, matching the current
  publication window.
- If publication order has hidden dependencies on non-modify memo entries, the
  hook/stress coverage should catch it; otherwise the change must be reverted.
- Larger remaining costs still sit in history-proof/native-support page
  publication, native commit MTR work, page-log append/encoding, and broader
  redo/checkpoint reconciliation.
