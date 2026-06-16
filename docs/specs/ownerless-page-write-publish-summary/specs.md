# Ownerless Page-Write Publish Summary

## Problem

The ownerless performance branch now separates WordPress PHPUnit test time
from build/setup time and emits production probe summaries for startup,
reconnect, read throughput, write throughput, page-log append work, and deep
InnoDB commit counters. The next remaining write-path target is narrower:
ownerless autocommit still spends material time in native mini-transaction
page publication and commit-log handling.

The detailed page-write counters already record the relevant subphases, but
the compact `mylite_perf_summary_*` rows used in CI only exposed aggregate
page-write publish time, buffer reuse, and the commit-MTR publish bucket. That
made CI summaries insufficient to tell whether a run was dominated by
dirty-page scan, page copy/checksum, page-version hook/page-log append,
redo-leave, or no-dirty commit-loop work.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` already records ownerless
  page-write performance counters for publish calls, scan time, deferred
  pages, tablespace lookup, allocation, copy, checksum, hook time, and
  commit-log subphases.
- `packages/libmylite/tests/embedded_performance_probe.c` already prints those
  counters in detailed raw output through `emit_page_write_stats()`.
- The same probe's ownerless autocommit phase summary previously emitted only
  aggregate publish time, publish buffer reuse hits/misses, and the
  commit-MTR publish bucket.
- `docs/COMPATIBILITY.md` and
  `docs/specs/ownerless-cross-process-concurrency/specs.md` identify native
  commit/page-publication and redo/checkpoint reconciliation as remaining
  ownerless performance and correctness work.

## Design

Promote the existing page-write detailed counters into per-insert summary rows
for the stats-enabled ownerless autocommit phase:

- publish calls,
- publish scan calls and time,
- deferred pages,
- tablespace lookup, allocation, copy, checksum, hook, and free time,
- commit-log calls,
- made-dirty and no-dirty commit-log calls,
- total commit-log time,
- flush-list, release, redo-leave, publish, release-memo, and no-dirty-loop
  time.

The existing `commit_mtr_publish_ms_per_insert` summary key remains for
continuity, while the new `page_write_commit_log_*` keys line up with the
detailed raw counter names. No new counters or hot-path probes are introduced;
the summary only derives additional rows from values already collected by the
stats-enabled performance run.

## Compatibility Impact

This is diagnostics-only. It does not change SQL behavior, public API
behavior, MariaDB compatibility, page-version WAL records, checkpoint records,
native page images, or ownerless visibility/recovery rules.

## Performance Impact

Stats-off production probes and normal runtime paths are unchanged. In the
stats-enabled ownerless attribution probe, the only added work is printing more
summary rows after the measured phase has completed.

## Test Plan

- Build `mylite_embedded_performance_probe` with the production embedded
  preset.
- Run a reduced stats-enabled ownerless performance probe and verify the new
  summary keys are emitted.
- Run the production-build audit and its CTest wrapper so timing-sensitive CI
  jobs remain guarded by production build types.
- Run formatting and whitespace checks.

## Acceptance Criteria

- The compact ownerless autocommit performance summary exposes the native
  page-write publish and commit-log subphase costs needed to pick the next
  write-path optimization from CI logs.
- Existing detailed metric keys and existing summary keys remain present.
- No ownerless page-version, checkpoint, recovery, SQL, or API behavior
  changes.

## Risks And Non-Goals

- This slice does not reduce the remaining ownerless write-path cost by
  itself. It makes the next optimization target visible in production CI
  summaries.
- This slice does not complete SQL-level table-lock fault injection, broader
  redo/checkpoint reconciliation, DDL/file lifecycle recovery, active-reader
  pressure policy, or external MariaDB/RQG stress coverage.
