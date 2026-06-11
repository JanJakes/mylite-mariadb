# Ownerless Page-Log Payload Attribution

## Problem

Production CI timing on `ownerless-concurrency` shows WordPress PHPUnit is not
currently slower than the measured main baseline once the suite is split from
build/setup work and all timing steps require production builds. The remaining
engine performance gap is the ownerless simple-autocommit write path.

The ownerless attribution probe already reports aggregate page-log append time
and payload bytes, plus InnoDB page-publish page-type counters. It does not
show how much encoded WAL payload belongs to each append encoding mode or
InnoDB page class. That leaves the next optimization ambiguous: the history
proof pages may need a role-specific representation, or the remaining bytes may
belong mostly to user/index pages.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` appends page-version WAL
  records through `append_record_at_locked()`, encoding full, trailing-zero, or
  sparse-zero payloads while preserving the existing 64-byte record header.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` classifies native-support page
  publication and proves the autocommit history fast path by publishing the
  rollback-segment SYS page and undo-header page.
- `packages/libmylite/tests/embedded_performance_probe.c` already reads the
  page-log append perf array and emits raw and per-insert ownerless attribution
  summaries.
- InnoDB page type constants used for attribution match
  `mariadb/storage/innobase/include/fil0fil.h`, including `FIL_PAGE_INDEX`,
  `FIL_PAGE_UNDO_LOG`, `FIL_PAGE_TYPE_SYS`, `FIL_PAGE_TYPE_TRX_SYS`,
  allocation/space-metadata pages, and BLOB pages.

## Design

Extend the existing stats-only page-log append counters. The append path will
record:

- encoding time,
- full, trailing-zero, and sparse-zero record counts and payload bytes,
- payload record counts and bytes for index, undo-log, SYS, TRX_SYS,
  allocation/space-metadata, BLOB, and other page classes.

Counters are collected only when the existing
`mylite_ownerless_page_log_set_append_perf_stats_enabled()` flag is enabled.
Normal ownerless writes keep the same WAL format, append ordering, locking,
checksums, and page-index publication.

The embedded performance probe will mirror the expanded counter enum, print raw
`mylite_perf_ownerless_insert_*_page_log_append_*` keys, and add per-insert
`mylite_perf_summary_ownerless_autocommit_page_log_*` keys so CI logs explain
which encoded page classes dominate the remaining write cost.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, directory-layout, or WAL-format
behavior changes. This is observability for ownerless performance probes.

## Directory And Lifecycle Impact

No files are added to a MyLite database directory. The existing
`concurrency/mylite-concurrency.wal` format, append order, checkpoint behavior,
and recovery semantics are unchanged.

## Native Storage Impact

Native InnoDB files and redo behavior are unchanged. This slice does not relax
the current history-proof requirement and does not claim broader native
redo/checkpoint reconciliation.

## Build And Performance Impact

Stats-disabled production writes only pay the existing relaxed-atomic flag
check. Stats-enabled attribution probes add page-type classification and
additional atomic increments while already collecting detailed probe data.

The motivating CI run `27381224991` reported WordPress test-only phase timings
of `9.476s` for `Tests_DB`, `85.111s` for deferred process-isolated tests,
`60.133s` for eager process-isolated tests, and `675.400s` for the
non-isolated remaining suite. The same run reported ownerless embedded
autocommit at `1033.24 ops/s` versus ordinary autocommit at `4165.13 ops/s` in
the stats-off probe, and the stats-enabled attribution sample reported
`7971.590` page-log payload bytes per insert, `0.073 ms/insert` in page-log
append time, and exactly one published history-proof rollback-segment page plus
one published history-proof undo page per insert.

A reduced local production attribution probe after this slice reported all
ownerless autocommit page-log records using sparse-zero encoding:
`3.020` sparse records per insert, `0.036 ms/insert` in encoding time, and
`7969.470` payload bytes per insert. The payload split was `3388.500`
index bytes, `4225.950` SYS bytes, `354.370` undo-log bytes, `0.650`
allocation/space-metadata bytes, and zero TRX_SYS/BLOB/other bytes per insert.
The new evidence points the next optimization toward the rollback-segment SYS
proof representation and user/index page payload, not the undo-header proof
payload.
A companion reduced stats-off production probe reported ownerless autocommit
at `1024.83 ops/s` versus ordinary autocommit at `3280.30 ops/s`, keeping the
normal write path in the same local range after the stats-only change.

## Tests And Verification Plan

- Build and run the ownerless primitive test so dense, sparse, and zero-page
  append stats are covered.
- Run a reduced production embedded attribution probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify the new raw and
  summary keys are emitted.
- Run focused ownerless CTest coverage for page-log and active-reader
  semantics if the probe changes expose a counter mismatch.
- Run CI production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Existing page-log append/read/checkpoint behavior is unchanged.
- Primitive tests prove aggregate payload bytes, encoding-mode counts/bytes,
  and page-type counts/bytes for representative dense, sparse, and all-zero
  pages.
- The embedded attribution probe emits per-insert page-log payload summaries by
  encoding mode and page type.
- The slice identifies the next ownerless write optimization target without
  weakening the history proof.

## Risks And Unresolved Questions

- The new stats do not by themselves reduce ownerless write time.
- Page-type attribution can identify SYS and undo-log payloads but does not
  distinguish every logical proof role without also consulting the InnoDB-side
  history-proof counters.
- A future optimization may still require a WAL-format or native redo proof
  change if the bytes remain concentrated in the two history-proof pages.
