# Ownerless History-Proof Native Reclaim

## Problem

Repeated ownerless `commit-race` runs exposed an intermittent InnoDB corruption
report after four independent ownerless writers committed explicit
transactions. The preserved failing directory had durable file-per-table row
pages whose row transaction identifiers were newer than the rollback-segment
history maximum recovered by native InnoDB startup. The ownerless WAL had
already been truncated to its empty recovery header, so the later opener had no
ownerless page-log evidence left to stop native reads from trusting stale
history metadata.

This slice tightens the no-live reclaim boundary: proof-only ownerless
rollback-segment and undo history records are not page images, but they are
still durable-native proof obligations. No-live reclaim must not clear the DML
marker or truncate the page-version WAL until the corresponding native pages
are flushed and can be verified from disk.

## Source Findings

- MariaDB base: 11.8 LTS, imported from `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0sys.h` documents that MariaDB 10.3.5
  and later recover the transaction id counter from
  `TRX_RSEG_MAX_TRX_ID` in rollback-segment headers plus undo-log history,
  not from the legacy `TRX_SYS_TRX_ID_STORE` field.
- `mariadb/storage/innobase/trx/trx0rseg.cc::trx_rseg_array_init()` scans
  rollback segments and calls `trx_sys.init_max_trx_id(max_trx_id + 1)`.
- `mariadb/storage/innobase/row/row0sel.cc` rejects clustered records and
  secondary pages whose transaction identifiers are greater than or equal to
  `trx_sys.get_max_trx_id()`, surfacing as InnoDB index corruption.
- `mariadb/storage/innobase/trx/trx0trx.cc::trx_t::write_serialisation_history()`
  can accept an ownerless WAL history proof and skip the exact native history
  page flush.
- `packages/libmylite/src/database.cc::collect_ownerless_native_page_checkpoint_record()`
  previously ignored `MYLITE_OWNERLESS_PAGE_LOG_RECORD_NATIVE_SUPPORT_STATE`
  records. `mylite_ownerless_page_log_replay_at()` also intentionally skipped
  proof-only records, so no-live native checkpoint proof never saw proof-only
  history obligations before truncating WAL.

## Design

- Add an internal page-log replay variant that includes proof-only records.
  Existing replay, page-index rebuild, latest-page reads, and checkpoint
  callbacks keep skipping proof-only records.
- During no-live native checkpoint proof, scan with the include-proof replay
  variant.
- Treat proof-only native-support records as native proof obligations. They
  carry page identity, page LSN, and commit LSN but no payload, so verification
  is native page-LSN based rather than byte-for-byte page comparison.
- Before verifying proof-only native-support records, force the owning native
  tablespace to flush dirty pages beyond the proof page LSN. This keeps the
  history WAL fast path but prevents DML marker drain and WAL truncation until
  MariaDB-native history metadata is durable.
- Keep live-peer reclaim conservative. The live-peer safety check also uses the
  proof-including scan, so proof-only history obligations block live reclaim in
  the same way payload records do.

## Scope

- First-party MyLite page-log API and no-live reclaim code.
- Primitive page-log coverage for include-proof replay.
- Focused ownerless SQL verification around `commit-race` and adjacent native
  reopen/reclaim selectors.

## Non-Goals

- No global disabling of ownerless history WAL proof.
- No broad rewrite of InnoDB redo or rollback-segment recovery.
- No new SQL surface or public `libmylite` API.
- No claim that broader DDL/file-lifecycle, active-reader pressure, or external
  randomized stress is complete.

## Compatibility Impact

Supported SQL behavior is unchanged. The compatibility claim becomes stronger:
ownerless DML reclaim may be slightly more conservative or perform close-time
native flush work, but it must not advertise committed ownerless row pages as
native-durable until InnoDB history metadata can survive an ordinary native
startup.

## Database Directory Impact

No files are added. Existing `concurrency/mylite-concurrency.wal` proof-only
records and `concurrency/mylite-concurrency.ckpt` DML markers remain inside the
MyLite database directory and are retained until native proof succeeds.

## Native Storage Impact

The slice protects InnoDB rollback-segment and undo history pages. It relies on
MariaDB's existing native flush machinery and verifies native page LSNs through
the ownerless InnoDB hook layer before reclaiming ownerless WAL.

## Binary Size And Dependencies

No new dependency. The page-log replay wrapper and reclaim checks add a small
first-party code path only.

## Test Plan

- Build `mylite_ownerless_primitives_test` and
  `mylite_ownerless_cross_process_sql_test` with the production embedded PHP
  preset.
- Run `mylite_ownerless_primitives_test`.
- Run `mylite_ownerless_cross_process_sql_test commit-race` repeatedly.
- Run the CTest shard that contains `commit-race`.
- Run focused adjacent ownerless native-support/history/DML marker selectors.
- Run `git diff --check`.

## Acceptance Criteria

- Proof-only native-support records can be included in a replay scan without
  changing normal replay/index/read behavior.
- No-live reclaim blocks or flushes until proof-only native-support history
  pages are native-durable.
- Repeated `commit-race` no longer produces InnoDB max-transaction-id/index
  corruption.
- Ownerless and ordinary native reopen, including forced `.shm` rebuild, still
  verify the committed rows.
- Documentation records that this closes a native history-proof reclaim gap,
  while broader ownerless concurrency work remains planned.

## Risks

- Close-time reclaim can now flush an undo tablespace when proof-only history
  records are present. This is a correctness-first cost and should be measured
  by the existing production performance probes after the fix.
- Native-support proof-only records without payload cannot prove bytes. The
  design therefore accepts native page-LSN proof only after forcing native
  flush and after the surrounding native checkpoint covers the visible LSN.
