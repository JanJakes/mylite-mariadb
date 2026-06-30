# Ownerless History Proof Payload Pair Contract

## Problem Statement

Ownerless history-proof pair publication is on the production stats-off
write path and is visible in the append-only CI attribution probe. Older slice
text described the pair as proof-only metadata, but later rollback-history
reclaim work restored payload-bearing native-support records so startup and
reclaim can validate exact rollback-segment and undo-header pages under DDL
startup storms.

The current contract needs executable coverage: the optimized pair hook may
coalesce the two proof publications, but it must still append native-support
records with page payloads until broader native redo/checkpoint reconciliation
replaces that exact-page evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` arms
  `mylite_ownerless_history_proof_active`, commits the history MTR, and accepts
  the fast proof only when the commit LSN is nonzero and both rollback-segment
  and undo-header proof pages were published.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_history_proof_publish_pair()` attempts the pair only for a
  nonzero commit LSN, distinct proof page identities, in-file non-temporary
  native-support pages, disabled detailed page-publish stats, and disabled
  unsafe test faults. If the pair succeeds, the normal per-page release loop
  skips those already-published proof pages; otherwise the existing per-page or
  native exact-flush fallback remains authoritative.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_history_proof_publish_pair_hook()` currently appends the
  rollback-segment and undo-header proof through `append_ownerless_page_version()`
  with `native_support_page=true` and without
  `MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY`, preserving payload-bearing
  page-log records.
- `packages/libmylite/src/ownerless_page_log.cc` still supports proof-only
  native-support records and a session-scoped proof-only pair primitive, but
  `docs/COMPATIBILITY.md` marks that representation as primitive machinery, not
  the current production rollback-history publication shape.

## Design

Add focused SQL coverage for the production pair contract:

- record the setup WAL baseline, then inspect the writer's page-version WAL
  before close-time no-live checkpoint/reclaim can drain proven native-support
  records;
- run inserts with database and deep InnoDB counters enabled, but detailed
  page-publish stats disabled so the production pair hook can run;
- assert the pair hook succeeds in normal production builds and remains disabled
  in unsafe hook builds;
- assert exact native history flush pages remain zero;
- assert the retained WAL contains native-support records and no native-support
  proof-only records;
- release the idle peer and verify final ownerless/native reopen preserves the
  rows while allowing rollback-history native-support WAL to remain only under
  the documented rollback-history retention rule.

This slice does not re-enable proof-only production history proof. It records
the current safety boundary so the next redo/checkpoint replacement slice has a
clear test to change deliberately.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol behavior, or MariaDB transaction
semantics change. The added coverage validates internal ownerless recovery
evidence and WAL record shape.

## Database Directory And Lifecycle Impact

No new files or directory-layout changes. The test inspects existing
`concurrency/mylite-concurrency.wal` records while an idle live peer retains
them and then verifies ordinary ownerless/native lifecycle reopening.

## Native Storage Impact

Native InnoDB files remain unchanged. Payload-bearing rollback/undo proof
records continue to serve as exact recovery evidence until native
redo/checkpoint reconciliation can prove a narrower representation.

## Build, Size, And Dependency Impact

No new dependency and no production binary-size change. The implementation is
test and documentation only.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the new focused selector and adjacent history/native-support selectors.
- Run the production ownerless SQL shard that contains the selector.
- Run the unsafe hook selector if the hook build is already available or touched.
- Run production build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Production focused coverage observes successful history-proof pair calls with
  zero exact native history flush fallback.
- The writer-open WAL contains newly appended payload-bearing native-support
  records and zero native-support proof-only records.
- Hook builds continue to bypass pair publication and rely on the existing
  per-page/fallback path.
- Docs no longer describe the production pair path as proof-only.

## Risks And Follow-Up

- This is evidence and contract hardening, not a speedup.
- The next higher-impact optimization remains replacing the exact
  rollback-history proof requirement through broader native redo/checkpoint
  reconciliation rather than by dropping payloads from the current proof.
