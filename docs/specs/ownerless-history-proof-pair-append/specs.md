# Ownerless History Proof Pair Append

## Problem Statement

Ownerless autocommit insert performance still pays per-commit overhead for the
two native history proof records that let MyLite skip the conservative exact
native rollback-segment and undo-page flush. Earlier slices experimented with
proof-only metadata records, but later rollback-history reclaim work restored
payload-bearing native-support records on the production history-proof path so
startup and reclaim can validate the exact rollback-segment and undo-header page
state.

This slice reduces that production/stats-off overhead by appending the two
existing payload-bearing native-support records through one narrow pair path
while preserving the current two-record WAL contract and native flush fallback.

## Source Findings

MariaDB base ref remains `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` arms
  `mylite_ownerless_history_proof_active` immediately before the history MTR
  commit and accepts the WAL proof only when `mtr->commit_lsn() != 0`, no page
  publish failed, and both rollback-segment and undo proof flags were set.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::commit_log()` writes `FIL_PAGE_LSN` while the modified page remains
  latched, publishes ownerless page versions before releasing the page latch,
  and then releases native MTR memo state.
- `mtr_t::ownerless_page_write_publish()` validates the page source, commit LSN,
  native-support page type, and history-proof role before calling
  `mylite_ownerless_innodb_publish_page_version_with_flags()`.
- `packages/libmylite/src/database.cc` `append_ownerless_page_version()` maps
  rollback-history native-support records onto the existing page-log append
  helpers with full page payloads and checksums on the current production path.
- `packages/libmylite/src/ownerless_page_log.cc` `append_record_at_locked()`
  still supports proof-only metadata records as primitive WAL machinery while
  preserving the existing payload-before-header, header-last crash ordering for
  payload-bearing records.

## Design

Add an optional ownerless InnoDB history-proof pair callback separate from the
existing single-page page-publish hook. The pair hook appends the two ordinary
payload-bearing native-support records:

- rollback-segment proof record with the existing history-rseg flag;
- undo-header proof record with the existing native-support flags.

The native MTR path attempts the pair only when all of these are true:

- ownerless hooks are active and the MTR has a nonzero commit LSN;
- the transaction has an active history proof with distinct rollback-segment
  and undo-header page numbers;
- both proof pages are still present in the current MTR memo;
- both pages are in-file, non-temporary, not transaction-lock-only pages, have
  their commit LSN installed, and have native-support page types;
- detailed ownerless page-publish stats are disabled;
- unsafe ownerless test-fault hooks are disabled.

When the pair hook returns OK, the native path marks both proof flags and skips
the later duplicate single-page proof publications in the normal release loop.
When the pair hook is unavailable, the native path does not mark proof state and
falls back to the existing per-page publication path. When the pair hook returns
an error, the native path marks page publication failed; the transaction then
uses the existing native exact history flush fallback because
`ownerless_history_wal_proved` remains false.

The pair path does not introduce a combined durable WAL format. It appends the
same two native-support page records through one optimized hook. The proof-only
record primitive remains available for lower-level page-log coverage, but
production rollback-history publication must stay payload-bearing until a
broader native redo/checkpoint proof can replace the exact rollback/undo page
evidence.

## Compatibility Impact

SQL, C API, PHP API, wire-protocol behavior, transaction semantics, and durable
database-directory layout are unchanged. The optimization affects only the
internal ownerless native history proof publication path.

Detailed page-publish attribution and unsafe ownerless fault builds continue to
use the existing per-page hook path so current crash-window tests and
page-publish proof counters remain exact.

## Native Storage Impact

The slice preserves MariaDB/InnoDB native storage files. It still relies on
InnoDB MTR page-latch ownership while installing the history MTR commit LSN and
appending proof metadata. If the pair cannot prove both pages, the existing
native exact rollback-segment/undo flush path remains the correctness fallback.

## Test And Verification Plan

- Extend `libmylite.ownerless-single-owner-history-wal-proof` so it first runs
  the detailed page-publish proof path, then runs additional inserts with only
  lightweight database/deep counters enabled and proves pair calls succeeded,
  pair failures/unavailable counts stayed zero, native-support proof skip
  accounting advanced, and exact native history flush pages stayed zero.
- Keep `libmylite.ownerless-single-owner-native-support-page-wal-elision`
  asserting disabled database counters stay zero, including the new pair
  counters.
- Add focused SQL coverage that records the setup WAL baseline, observes the
  writer-open WAL before close-time reclaim, and proves the production pair
  appends payload-bearing native-support records with zero native-support
  proof-only records.
- Build production embedded targets after rebuilding the MariaDB embedded
  archive because the slice edits InnoDB-derived files.
- Run focused ownerless primitives/history/native-support/hook verification,
  a stats-off production performance probe, format check, and `git diff
  --check`.

## Acceptance Criteria

- Pair proof publication is optional and fails closed to existing behavior.
- The fast proof is accepted only after both native-support records append
  successfully.
- Existing unsafe ownerless fault builds retain the per-page publish path.
- The focused SQL proof shows pair activation without exact history flush
  fallback.
- Focused WAL-shape coverage proves production retained history proof remains
  payload-bearing and does not silently switch back to proof-only metadata.
- No public API, SQL behavior, durable file format, or directory-layout claim
  changes.

## Risks And Follow-Up

- The pair path removes duplicate hook calls but not the two durable proof
  records themselves, so throughput gains may be modest and noisy.
- Broader native redo/checkpoint reconciliation could replace more of the
  current proof machinery, but that remains a separate correctness slice.
- A future unsafe-hook slice can add explicit pair-specific crash windows if
  the pair path becomes enabled in hook-test builds.
