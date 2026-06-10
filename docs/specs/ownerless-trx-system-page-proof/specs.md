# Ownerless TRX System Page Proof

## Problem

The timer checkpoint scheduling fix removed background buffer-pool scan
publication from tight ownerless write loops. The remaining production
performance gap is now dominated by ownerless write-path proof work that still
publishes full page images. The earlier stats-enabled attribution probe showed
one ownerless `trx_system` native-support bucket page and one undo
native-support page still published per autocommit insert, plus one ordinary
user-page snapshot record.

The undo page is part of the existing history proof and is not the first safe
optimization target. The `trx_system` bucket was the plausible next target,
but the bucket originally conflated MariaDB `FIL_PAGE_TYPE_SYS` and
`FIL_PAGE_TYPE_TRX_SYS`. MyLite must not elide either ownerless page-version
WAL image until the actual page type, changed fields, and peer-reader
requirements are proven.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines
  `FIL_PAGE_TYPE_SYS = 6` and `FIL_PAGE_TYPE_TRX_SYS = 7`.
- `mariadb/storage/innobase/include/trx0types.h` defines
  `TRX_SYS_SPACE = 0` and `TRX_SYS_PAGE_NO = FSP_TRX_SYS_PAGE_NO`.
- `mariadb/storage/innobase/include/trx0sys.h` documents the transaction
  system page layout. MariaDB 10.3.5 and later no longer use
  `TRX_SYS_TRX_ID_STORE` as the live max transaction id source, but the page
  still contains rollback-segment slot metadata, legacy binlog/WSREP upgrade
  fields, and doublewrite-buffer metadata.
- `mariadb/storage/innobase/trx/trx0sys.cc::trx_sys_create_sys_pages()`
  creates the `FIL_PAGE_TYPE_TRX_SYS` page and initializes rollback-segment
  slot metadata.
- `mariadb/storage/innobase/trx/trx0rseg.cc` still reads
  `TRX_SYS_TRX_ID_STORE` for upgrade compatibility and reads rollback-segment
  slots from the TRX_SYS page while restoring rollback segment state.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_publish_type_has_native_support()` classifies
  both `FIL_PAGE_TYPE_SYS` and `FIL_PAGE_TYPE_TRX_SYS` as native-support
  state. Older ownerless stats grouped both under the `trx_system` bucket.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` only elides
  native-support pages in the transaction rollback segment's own undo
  tablespace and explicitly keeps active history-proof pages. TRX system pages
  in `TRX_SYS_SPACE` therefore remain published today.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_page_type_is_native_support_state()` treats `FIL_PAGE_TYPE_TRX_SYS`
  records as native-support records for retention-boundary purposes after they
  have been written. This is not the same as proving the record can be skipped.

## Current Evidence

CI run `27302225684` on `64e48a6e` used production build guards:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

The default embedded production probe reported:

- ordinary autocommit inserts: `2568.21 ops/s`,
- ownerless autocommit inserts: `707.17 ops/s`,
- ownerless autocommit ratio: `0.2754`,
- ordinary warm open/close: `272.969 ms`,
- ownerless warm open/close: `271.761 ms`,
- ownerless active-runtime reconnect: `0.328 ms`.

The stats-enabled ownerless attribution probe reported:

- `3.000` MTR-published page-version records per insert,
- `3.030` total page-publish hook calls per insert,
- `0.030` extra page-publish hook calls per insert,
- `0.000` buffer-pool scan publishes per insert,
- `2.000` published native-support pages per insert,
- `1.000` published undo page per insert,
- `1.000` published trx-system bucket page per insert,
- `1.000` non-native-support page per insert,
- `0.000` actual synthesized snapshot-boundary pages per insert,
- `3.020` page-log append calls per insert,
- `49672.960` page-log bytes per insert,
- `0.076 ms/insert` in page-write publish,
- `0.048 ms/insert` in page-log append.

The diagnostic attribution probe is intentionally heavier than the default
probe and should be used for source attribution, not as the primary throughput
number.

The first implementation split the aggregate `trx_system` bucket into concrete
`FIL_PAGE_TYPE_SYS` and `FIL_PAGE_TYPE_TRX_SYS` counters and added a
diagnostic-only byte-diff helper for the canonical transaction-system page
identity: `space_id == TRX_SYS_SPACE`, `page_no == TRX_SYS_PAGE_NO`, and
`page_type == FIL_PAGE_TYPE_TRX_SYS`. A reduced local production attribution
sample on 2026-06-10 with `100` ownerless autocommit inserts reported:

- `1.000` published native-support `trx_system` bucket page per insert,
- `1.000` published `FIL_PAGE_TYPE_SYS` page per insert,
- `0.000` published `FIL_PAGE_TYPE_TRX_SYS` pages per insert,
- `0.190` elided `FIL_PAGE_TYPE_SYS` pages per insert,
- `0.000` elided `FIL_PAGE_TYPE_TRX_SYS` pages per insert,
- `0.000` canonical TRX_SYS byte-diff samples per insert,
- `49672.960` page-log bytes per insert,
- ownerless autocommit at `710.08 ops/s` versus ordinary autocommit at
  `2047.17 ops/s` in the stats-enabled diagnostic run.

That result corrects the earlier interpretation: the remaining measured
`trx_system` bucket publication in this autocommit path is a generic
`FIL_PAGE_TYPE_SYS` page, not the canonical InnoDB transaction-system page.
TRX_SYS page elision is therefore not the next measured hot-path optimization
for this workload; the next evidence slice must identify the `FIL_PAGE_TYPE_SYS`
page identity and source semantics before any elision is considered.

## Scope And Non-Goals

In scope:

- Prove exactly which TRX_SYS page bytes change during the measured ownerless
  autocommit insert path.
- Distinguish `FIL_PAGE_TYPE_SYS` from `FIL_PAGE_TYPE_TRX_SYS` inside the old
  aggregate ownerless `trx_system` bucket before treating the bucket as
  canonical transaction-system state.
- Decide whether those bytes are required for peer current-read visibility,
  repeatable-read page-version replay, native checkpoint/recovery, rollback
  segment restoration, doublewrite state, or upgrade compatibility.
- Add instrumentation or focused primitive coverage that distinguishes
  safe-to-elide trx-system images from required trx-system proof images.
- Preserve the existing undo/history proof behavior.

Out of scope for the first implementation:

- Blindly eliding all `FIL_PAGE_TYPE_TRX_SYS` page-version WAL records.
- Changing page-version WAL format or crash-tail record ordering.
- Replacing full-page native-support records with deltas.
- Broader DDL/file-lifecycle or external MariaDB/RQG stress coverage.
- Changing MariaDB transaction-system page format.

## Proposed Design

Start with a proof/instrumentation slice, not an elision slice.

The proof should classify a published TRX_SYS image by stable page identity:
`space_id == TRX_SYS_SPACE`, `page_no == TRX_SYS_PAGE_NO`, and
`page_type == FIL_PAGE_TYPE_TRX_SYS`. For those records, the performance probe
should expose whether the page was published by the normal MTR path and how
many such records were accepted into the page-version WAL.

The implementation must also keep the old `trx_system` aggregate counter for
log continuity while exposing the underlying `FIL_PAGE_TYPE_SYS` and
`FIL_PAGE_TYPE_TRX_SYS` counts separately for published and elided
native-support pages.

An implementation may then add a local comparison helper that reads the latest
native TRX_SYS page image at statement boundaries or commit-time publication
points and reports the changed byte ranges relative to the previous accepted
TRX_SYS image. That helper must be diagnostic-only until the changed ranges are
mapped to MariaDB source semantics.

Only after the changed ranges are proven redundant for ownerless peer
visibility and recovery should a later slice add an elision predicate. That
predicate must be narrower than the generic native-support page elision and
must leave the existing undo history-proof pages published.

## Compatibility Impact

The proof slice does not change SQL, C API, PHP API, wire-protocol behavior, or
MySQL/MariaDB compatibility. A future elision slice would also have to preserve
MariaDB transaction, rollback-segment, and crash-recovery behavior for native
InnoDB files inside the MyLite database directory.

## Database Directory And Lifecycle Impact

The proof slice should not add durable files or change the MyLite database
directory layout. Any diagnostic counters are process-local. Any future elision
must preserve forced `.shm` rebuild, reopen, native checkpoint, and page-version
WAL reclaim behavior.

## Native Storage Impact

No native storage format change is allowed. The TRX_SYS page remains an
InnoDB-native page in the system tablespace. The only acceptable optimization
path is skipping redundant MyLite page-version WAL records when native InnoDB
state already proves the same visibility and recovery boundary.

## Build, Size, And Dependency Impact

The proof slice should add no dependencies and no production binary-size
material beyond optional diagnostic counters or narrowly scoped helper code.
Any stats-only code must keep the existing production-build timing guards.

## Test And Verification Plan

- Build production embedded targets with `build/mariadb-embedded` as
  `MinSizeRel` and `build/php-embedded-prod` as `Release`.
- Extend the embedded performance probe or ownerless primitive tests with
  parseable TRX_SYS publish counters.
- Run a stats-enabled ownerless attribution probe and prove whether the old
  `trx_system` bucket is `FIL_PAGE_TYPE_SYS`, `FIL_PAGE_TYPE_TRX_SYS`, or both
  before any elision.
- Add focused coverage that forces a `.shm` rebuild after ownerless autocommit
  insert loops and verifies committed data survives reopen.
- Run live peer reader/writer visibility coverage with a repeatable-read pin,
  post-release checkpoint, and final reclaim.
- Run existing single-owner native-support and history-WAL proof selectors.
- Run hook crash selectors around page publish, redo-written/visible, and
  pages-visible checkpoint before claiming elision safety.
- Run `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- The spec identifies the old `trx_system` bucket publication as the next
  measured performance target after timer-driven buffer-pool scan publication
  was removed.
- Instrumentation can distinguish `FIL_PAGE_TYPE_SYS`,
  `FIL_PAGE_TYPE_TRX_SYS`, and undo history-proof publication.
- Any implementation keeps undo history-proof pages published.
- No elision is accepted until tests prove peer visibility, recovery, forced
  shared-memory rebuild, and reclaim behavior with TRX_SYS records omitted.
- Production CI continues to use production build directories before reporting
  timings.

## Risks And Unresolved Questions

- The TRX_SYS page contains legacy upgrade fields and doublewrite metadata, so
  changed bytes may be benign for ordinary MyLite workloads but still required
  for native recovery or fork compatibility.
- The current probe now proves the measured hot `trx_system` bucket record is
  `FIL_PAGE_TYPE_SYS` in the reduced autocommit sample. A follow-up helper is
  needed to identify that page's stable identity and semantics before changing
  behavior.
- Even a safe TRX_SYS elision would not improve the measured autocommit hot
  path while canonical TRX_SYS samples remain zero; the generic
  `FIL_PAGE_TYPE_SYS` page and undo proof page remain the measured
  native-support publication work.
