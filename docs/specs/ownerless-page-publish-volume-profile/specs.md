# Ownerless Page-Publish Volume Profile

## Problem

The ownerless page-log batch-append slice reduced append-lock and `fstat()`
overhead, but production profiling still shows ownerless autocommit inserts
are much slower than the ordinary path. The remaining page-log cost is mostly
page-image volume: the reduced stats-enabled 400-row autocommit sample
published 3203 page-version records, with 2803 classified as native-support
pages and 400 classified as snapshot-boundary-required pages.

Before suppressing native-support page records, MyLite needs evidence about
duplicate page identities. If many records repeat the same
`(space_id,page_no,visible_lsn)`, a later correctness slice can investigate
coalescing. If the records are mostly unique, the next optimization must change
the native redo/checkpoint contract or reduce the set of page types that need
publication.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` is the commit-MTR publication point
  for native-support pages and immediate page-version records.
- Existing page-publish stats classify candidates, publishes, skipped records,
  InnoDB page type groups, and native-support versus snapshot-boundary-required
  pages, but they do not identify repeated page identities.
- The transaction-deferred user/data page path already deduplicates
  transaction-held pages by `(space_id,page_no)` before commit publication.
  The remaining hot path is mini-transaction-scope native-support publication.

## Design

Add stats-enabled identity counters to `mtr0mtr.cc`:

- `page_publish_identity_unique`: first observed
  `(space_id,page_no,visible_lsn)` fingerprint in the current stats interval;
- `page_publish_identity_duplicate`: repeated fingerprint in the current stats
  interval;
- duplicate breakdown by native-support versus snapshot-boundary-required page
  class;
- duplicate breakdown by index, undo, space-metadata, transaction-system, blob,
  and other page type groups;
- `page_publish_identity_table_overflow`: bounded fingerprint table saturation
  evidence.

The implementation uses a fixed-size open-addressed fingerprint table with
relaxed atomics and no dynamic allocation. It runs only when existing
page-publish stats are enabled, preserving the stats-off hot path as the
current relaxed flag check. Fingerprints are diagnostic, not a correctness
source; collisions are possible in theory and are acceptable for profiling.

Expose the new counters through the existing
`mylite_ownerless_innodb_read_page_publish_stats()` array and print them from
`mylite_embedded_performance_probe` when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or runtime behavior changes.
The new counters are diagnostic output in an existing internal performance
probe path.

## Directory And Lifecycle Impact

No durable files or directory layout changes. The fingerprint table is
process-local and is reset with the existing page-publish stats reset call.

## Native Storage Impact

No InnoDB page, redo, undo, dictionary, or checkpoint format changes. This
slice only records diagnostic evidence.

## Performance Impact

Stats-off production behavior is unchanged. Stats-on probes pay extra atomic
loads, compare/exchange probes, and bounded linear probing per published page,
which is acceptable because stats-on runs are diagnostic and already slower
than production.

Local reduced production evidence showed the page-version volume is almost
entirely unique by `(space_id,page_no,visible_lsn)`: 400 ownerless autocommit
inserts published 3203 page-version records, with 3198 unique identities and
only 5 duplicates. All 5 duplicates were native-support records: 2 undo pages
and 3 transaction-system pages. The fingerprint table did not overflow.

This makes simple same-visible-LSN duplicate suppression an unattractive next
optimization target. Reducing the ownerless autocommit gap requires changing
which native-support page classes must be published, or proving stronger native
redo/checkpoint reconciliation, rather than coalescing repeated page identities.

## Test Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Rebuild production embedded performance-probe and ownerless SQL targets.
- Run reduced stats-on and stats-off production performance probes and confirm
  the new identity counters are emitted only in stats-on output.
- Run focused production ownerless SQL selectors and ownerless primitives.
- Run `ownerless-test-hooks` negative proof, `ownerless-stress`,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Stats-on ownerless autocommit output reports unique and duplicate page
  identity counts with native-support/type breakdown.
- Stats-off production probe behavior remains functional.
- No production code path depends on the diagnostic fingerprint table for
  correctness.
- Docs state whether the measured page-version volume is mostly duplicate or
  mostly unique.

## Verification Results

Local production verification on 2026-06-08:

- `tools/mariadb-embedded-build build`: passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test`: passed.
- Reduced stats-on production probe with 400 insert iterations passed and
  reported `page_publish_published=3203`, `page_publish_native_support=2803`,
  `page_publish_snapshot_boundary=400`,
  `page_publish_identity_unique=3198`,
  `page_publish_identity_duplicate=5`,
  `page_publish_identity_duplicate_native_support=5`,
  `page_publish_identity_duplicate_type_undo=2`,
  `page_publish_identity_duplicate_type_trx_system=3`, and
  `page_publish_identity_table_overflow=0`.
- Reduced stats-off production probe passed; ordinary autocommit measured
  `1990.59 ops/s` and ownerless autocommit measured `226.97 ops/s` in that
  noisy 400-row sample.
- `ctest --preset php-embedded-prod -R 'libmylite\.ownerless-primitives$'
  --output-on-failure`: passed.
- Focused production ownerless SQL selectors passed:
  `prepared-committed-read`, `local-write-first-read`, `native-reclaim`,
  `live-reclaim`, `commit-race`, and `active-reader-pressure`.
- Production direct SQL case 129
  `test_ownerless_descending_primary_key_ddl_refreshes_peer_dictionary`:
  passed.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure`: passed.
- `ctest --preset ownerless-stress --output-on-failure`: passed all 12 cases.
- `cmake --build --preset php-embedded-prod`: passed.
- `cmake --build --preset format`, `cmake --build --preset
  format-check-prod`, and `git diff --check`: passed.
