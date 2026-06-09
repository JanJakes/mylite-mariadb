# Ownerless Page-Version Peer Handoff Proof

## Problem

Production ownerless profiling identified rollback-segment history flushing as
a major autocommit cost. A prototype that skipped the native history-space
flush under the existing continuous single-owner proof reduced that cost, but
ownerless independent-table stress then exposed a deeper correctness problem:
readers could observe one table value move backward while other writers were
committing.

The final slice therefore keeps the native history flush and fixes the
page-version handoff evidence that made performance timings unsafe to trust.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` still needs the native
  rollback-segment tablespace flush as part of the current peer handoff proof.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` owns page-version publication
  after mini-transaction page LSNs are installed. Transaction-deferred writes
  also need a pre-modification boundary image so a peer reader can select the
  committed page image while another process holds the page-write lock.
- `packages/libmylite/src/database.cc` page-version reads use the shared
  page-version index as an acceleration cache. The WAL append happens before
  the page-index publish, so a peer can observe a newer WAL record before the
  index points at it. Direct indexed hits must therefore validate the WAL tail
  before returning.
- The shared visible LSN can remain numerically unchanged while a new visible
  page boundary is published. Readers need a visible-generation signal in the
  redo state, not only an LSN comparison.
- Retained page-version WAL can contain snapshot-boundary images, not only the
  newest native page image. Equal InnoDB page LSNs therefore cannot prove that a
  retained ownerless image should replace a native buffer or disk page.

## Design

- Keep the native history-space flush unchanged.
- Track a shared redo visible generation and refresh clean ownerless reader
  pages when either the visible LSN, visible generation, or process-registry
  generation changes.
- Publish transaction-deferred pre-write boundary page images at their current
  page LSN so a reader can choose the last committed page image while a peer
  modifies the page.
- Publish the non-fast commit visible boundary with the physical flush LSN when
  the flush path advanced beyond the SQL commit LSN.
- Treat the page-version index as a cache. On a direct indexed hit, read the
  indexed record, snapshot the WAL under the page-log read guard, and scan only
  the tail after the indexed record. A newer visible tail record wins; otherwise
  the indexed page remains the fast result.
- Do not overlay same-LSN page-version images during live clean-page refresh.
  During product no-live tablespace replay, keep an existing native disk page
  whose page LSN equals the retained ownerless image.
- Before no-live reclaim discards retained ownerless page WAL, publish the
  current buffer-pool page set to the reclaim LSN, wait for dirty native pages
  through that LSN, and then take the native checkpoint. This keeps FK cascade
  and DDL side-effect pages durable in native storage before the retained
  snapshot-boundary records are compacted.
- Keep ownerless stress checking each independent table monotonically, not only
  the aggregate total, so a one-table regression cannot be hidden by another
  writer's progress.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, or storage-format behavior
changes. The slice strengthens ownerless read consistency for non-locking
`SELECT`/`WITH` page-version reads and documents that the current
single-owner proof is not enough to skip native history flushing.

## Directory And Lifecycle Impact

No durable files or directory layout are added. The redo-state shared-memory
segment version is bumped for the visible-generation field; incompatible stale
volatile `.shm` state is rebuilt through the existing ownerless recovery path.

## Native Storage Impact

Rollback-segment history pages remain native-flushed through the history MTR
LSN. Page-version WAL records remain the peer read boundary, with additional
pre-write boundary records for transaction-deferred page writes and WAL-tail
validation for index hits. No-live reclaim now flushes native dirty pages
through the page-visible reclaim LSN before checkpointing and compacting
retained page-version WAL.

## Build And Performance Impact

The rejected history-flush optimization remains a negative result. The WAL-tail
validation adds a bounded tail scan after direct page-index hits only when the
WAL snapshot has bytes beyond the indexed record. This is a correctness cost on
the ownerless reader path and should be watched in production performance
probes, but it avoids replacing indexed reads with full WAL scans. The no-live
flush step adds close/reclaim work only when retained page-version WAL is being
made native-durable; it prevents correctness loss that would otherwise make
performance timings untrustworthy. The post-fix production sample under
`php-embedded-prod` reported stats-off ownerless warm open/close at
`359.230 ms` versus ordinary `375.478 ms`, active-runtime reconnect overhead at
`0.211 ms`, ownerless direct/prepared read ratios of `0.9008`/`0.8629`,
transactional insert ratio of `0.7110`, and autocommit insert ratio of
`0.3008`. The reduced stats-enabled attribution run still showed the remaining
autocommit gap in page-version publication and native history proofing:
`4.570` page-version records per insert, `3.570` native-support records per
insert, `0.433 ms/insert` in page-log append, and `0.561 ms/insert` in the
rollback-segment-space dirty-page flush.

## Test Plan

- Add and run `single-owner-history-flush-native-proof`.
- Strengthen and run `single-owner-skip-peer-history`.
- Add and run `uncommitted-peer-hidden`.
- Strengthen and run `ownerless-cross-process-stress` with per-table
  monotonic reader checks.
- Run focused ownerless SQL selectors affected by page-version and reclaim
  behavior.
- Run DDL/FK-focused coverage including `ddl-broader`, DDL stress, and FK graph
  stress so retained boundary records cannot resurrect cascaded rows after
  ownerless/native reopen.
- Run ownerless primitive and hook subsets, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- A peer's uncommitted update stays hidden and becomes visible after commit on
  the same reader handle.
- Independent-table stress readers never observe per-table values moving
  backward.
- Single-owner and stale-peer history tests prove native history flushes still
  occur where the current handoff requires them.
- Docs record why the page index is not authoritative without a WAL-tail check.
- Broader DDL/FK coverage proves no-live reclaim makes native side-effect pages
  durable before retained page-version WAL is discarded.

## Risks And Follow-Up

- The ownerless autocommit performance gap remains; the native history flush
  and page-version write volume still need separate optimization.
- SQL-level table-lock fault injection remains unproven because explored SQL
  shapes have not reached the ownerless table-wait callback.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  active-reader pressure policy, DDL/dictionary/space allocation classes, and
  external MariaDB/RQG stress remain planned follow-up evidence.
