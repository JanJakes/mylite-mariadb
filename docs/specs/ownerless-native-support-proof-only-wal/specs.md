# Ownerless Native-Support Proof-Only WAL

## Problem

Ownerless single-owner autocommit inserts still publish two native-support
history-proof page images for each eligible commit: the rollback-segment
`FIL_PAGE_TYPE_SYS` page and the undo-header page. These records prove that
the native InnoDB history update was published before the fast ownerless
visibility boundary is released, but readers must not fetch these support
pages from the ownerless page-version index. The current representation writes
the full page payload anyway, so it pays page-copy, page-checksum, page-log
encoding, and payload-write cost for records whose durable role is proof
metadata.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc:1485` implements
  `trx_t::write_serialisation_history()`. It marks
  `mylite_ownerless_history_proof_active` before committing the history
  mini-transaction and requires both rollback-segment and undo proof pages to
  be published before using the fast proof.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:2806` publishes modified pages from
  `mtr_t::ownerless_page_write_publish()`. The existing path copies the page
  into a scratch buffer, prepares it for writing, then calls
  `mylite_ownerless_innodb_publish_page_version_with_flags()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:1284` blocks ordinary elision for
  active history-proof pages. That keeps the proof pages durable but also makes
  them the remaining native-support WAL payload target.
- `packages/libmylite/src/database.cc:16794` implements
  `ownerless_innodb_page_publish_hook()`. It already skips live page-index
  publication for native-support pages after appending their WAL records.
- `packages/libmylite/src/ownerless_page_log.cc:6404` validates page-log
  payload shapes. Existing records require a readable payload unless they use
  a supported delta/sparse encoding.
- `packages/libmylite/src/ownerless_page_log.cc:3675` replays valid WAL records
  into callers such as the `.shm` page-index rebuild, and
  `packages/libmylite/src/ownerless_page_log.cc:3749` rewrites retained records
  during checkpoint. A proof-only record must be explicit so replay can skip it
  while checkpoint can still preserve it under existing retention rules.

## Design

Add an explicit page-log metadata flag for proof-only records. A proof-only
record carries page identity, page LSN, commit LSN, native-support metadata,
and zero payload bytes. It is valid only with the native-support append option,
has no payload encoding flag, and is not readable as a page image.

Add an explicit InnoDB publish flag for proof-only native-support records. The
MTR history-proof path uses it only when the modified page is an active
history-proof rollback-segment or undo page and its page type is already one of
the native-support page types. In that path, InnoDB calls the publish hook with
the original page pointer and skips scratch-buffer allocation, page copying,
and page-write checksum preparation. The MyLite page-log hook then skips
payload checksum calculation and appends a proof-only native-support record.

Page-log read, latest-scan, replay, and checkpoint semantics are:

- `find_latest` ignores proof-only records as page-image candidates;
- direct record reads reject proof-only records as not found;
- replay callbacks skip proof-only records so page-index rebuilds never point
  at unreadable proof metadata;
- checkpoint retention may keep proof-only records, but retained-record
  callbacks skip them for the same page-index reason;
- native checkpoint proof collection continues to skip native-support records
  by metadata without decoding payloads.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, DDL, or storage-engine behavior changes.
Ownerless history-proof publication remains required for the same commit
shapes; only the internal WAL representation changes for those proof records.

## Directory And Lifecycle Impact

No new files or directory-layout changes. Existing
`concurrency/mylite-concurrency.wal` records gain a metadata shape that older
MyLite builds on this development branch would not understand.

## Native Storage Impact

Native MariaDB/InnoDB files and page formats are unchanged. The slice relies on
the existing native history-space flush and the already-required proof page
publication ordering; it does not broaden native checkpoint proof or replace
redo recovery.

## Performance Impact

The targeted win is per-insert page-log payload work: two native-support
history-proof records no longer copy, prepare, checksum, encode, or write 16
KiB page payloads. Record headers are still appended and synced through the
existing page-log path.

A reduced stats-enabled production probe over 100 ownerless autocommit inserts
reported `2.000` published native-support history-proof records per insert,
`1.000` rollback-segment proof record, and `1.000` undo proof record. Because
those proof records were proof-only, the ownerless page-write publish path
reported `0.000 ms` for scratch allocation, page copy, and checksum
preparation, and page-log append reported `503.380` payload bytes per insert.
The same stats-enabled sample is attribution evidence; throughput comparisons
come from the stats-off sample below.

A matching 500-row stats-off sample reported ownerless direct `SELECT 1` at
`0.8066x` ordinary, ownerless prepared `SELECT 1` at `0.8669x` ordinary,
ownerless explicit transactions at `0.6455x` ordinary, ownerless autocommit at
`0.5478x` ordinary, and ownerless four-row bulk rows at `0.4154x` ordinary.
These are local production samples, not a CI-sized performance claim, but they
show the proof-payload work no longer dominates the WAL payload summary.

## Tests And Verification

- Add primitive page-log coverage for proof-only append shape, metadata,
  latest/read rejection, replay skipping, and checkpoint callback skipping.
- Extend focused ownerless SQL history/native-support tests to prove the
  existing history-proof counters still publish rollback-segment and undo proof
  records.
- Run production embedded performance probes with page-publish stats to confirm
  page-log payload bytes and append/checksum timings fall while correctness
  selectors pass.

## Acceptance Criteria

- Proof-only records are accepted only as native-support metadata records.
- Page reads never return proof-only records as page images.
- Page-index replay and checkpoint retained callbacks never index proof-only
  records.
- Existing ownerless history-proof selectors continue to pass.
- Production performance output documents the before/after effect on ownerless
  autocommit insert WAL payload work.

## Verification

- `tools/mariadb-embedded-build build` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_negative_proof_test mylite_ownerless_cross_process_sql_test`
  passed after rebuilding the hook preset.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` passed for all 16 registered ownerless SQL shards.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed.
- `tools/check-ci-production-builds` and `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.
- Reduced production performance probes were run with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=100`,
  and `MYLITE_PERF_INSERT_ITERATIONS=100` plus
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and with
  `MYLITE_PERF_INSERT_ITERATIONS=500` stats-off for throughput comparison.
- `ctest --preset ownerless-stress --output-on-failure` is not green on this
  branch: the amplified DDL and temporary-table stress cases intermittently hit
  MariaDB/InnoDB `trx0trx.cc:1337` (`trx->error_state == DB_SUCCESS`) or child
  aborts. The default direct `ddl-stress` and `temp-stress` commands passed,
  and direct eight-round `ddl-stress` passed under both production and
  ownerless-stress binaries, so the failing evidence remains a broader
  ownerless stress gap rather than proof-only WAL replay coverage.

## Risks

- This is a WAL format extension inside a development branch. Mixed binaries
  from before this slice cannot read proof-only records.
- It does not solve broader native redo/checkpoint reconciliation, DDL/file
  lifecycle recovery, SQL-level table-wait coverage, or external RQG stress.
