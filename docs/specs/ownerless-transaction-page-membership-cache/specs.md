# Ownerless Transaction Page Membership Cache

## Problem

The 2048-row visible-fast append batch proved that pure ownerless
`INSERT ... VALUES` statements can keep one page-log append session and defer
latest-checkpoint updates across a larger statement. The same production probe
also showed that later statements still spend most of their ownerless overhead
inside native row insert and undo-report mini-transactions, not page-log append
session churn.

Some of that later-statement work repeatedly tests whether a transaction
already owns or dirtied a packed `(space_id, page_no)` page. Before this slice,
those checks linearly scanned the transaction's ownerless modified/dirty page
vectors from the MTR and lock-hook paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0trx.h` stores ownerless
  transaction-deferred modified pages and dirty pages as
  `trx_t::mylite_ownerless_page_vector` pointers. Those vectors remain the
  authoritative ordered storage for cleanup, collection, and gate erasure.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_transaction_owns_page()`,
  `ownerless_page_write_enter()`,
  `ownerless_page_write_release_deferred()`,
  `ownerless_page_write_note_transaction_page()`, and
  `ownerless_page_write_note_dirty_transaction_page()` repeatedly test exact
  page membership on those vectors.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `transaction_has_page_write_gate()`,
  `transaction_has_page_write_entry()`,
  `note_transaction_page_write_gate()`, and
  `note_transaction_page_write_page()` make the same exact membership checks.
- MariaDB's current bulk-buffer path remains tied to the first statement that
  inserts into an empty table with `TRX_UNDO_EMPTY`; later non-empty inserts
  still require row-level undo for statement rollback and savepoint semantics.

## Design

Add lazy exact membership caches to `trx_t` for ownerless modified pages and
dirty pages:

- keep the existing vectors as the authoritative source;
- build an open-addressed set only after the vector reaches 16 entries;
- use the set only for exact membership checks, with no false positives;
- update the set when a new unique page is appended;
- grow or rebuild before the set exceeds half capacity;
- clear caches when the vectors are cleared;
- rebuild the modified-page cache after transaction page-write gates are
  erased.

The set is process-local transient state. It does not change page-write lock
ownership, page-version WAL, checkpoint, recovery, native InnoDB page format,
or SQL behavior.

## Compatibility Impact

No SQL syntax, public C API, PHP/mysqli behavior, wire-protocol behavior,
storage format, or directory layout changes. The same writes remain visible
and durable under the existing ownerless publication policy.

The slice deliberately does not broaden MariaDB's default-checked bulk insert
path for non-empty tables. Later 2048-row statements still report
`2048.000` undo-report calls per statement and `0.000` default-checked bulk
starts per statement.

## Directory And Lifecycle Impact

No durable files, shared-memory fields, lock bytes, or cleanup lifecycle rules
are introduced. Cache memory is allocated only inside the owning process's
`trx_t` and is cleared with the existing ownerless transaction page vectors.

## Native Storage Impact

No InnoDB page, redo, undo, or checkpoint format changes. The optimization
only reduces repeated process-local membership scans before existing
page-write lock and deferred-publication decisions.

## Build And Performance Impact

The implementation touches upstream-derived InnoDB transaction, MTR, and
ownerless lock-hook files, so the MariaDB embedded archive must be rebuilt.

The reduced 2048-row production probe before this slice reported later
ownerless statements at `0.3195` of ordinary bulk rows throughput, with
`55.134 ms` remaining row-insert time and `16.498 ms` remaining
undo-report MTR commit time per statement.

After the cache, the same reduced probe reported later ownerless statements at
`0.3347`, with `51.364 ms` remaining row-insert time and `14.724 ms`
remaining undo-report MTR commit time per statement. This is a bounded
improvement; the remaining target is still native row-insert/undo-report work,
not append-session churn.

## Verification Plan

- Rebuild the MariaDB embedded archive.
- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused
  `single-owner-multi-row-insert-visible-fast-path` selector.
- Run adjacent ownerless selectors covering history proof, native-support page
  publication, FK fast-path cache invalidation, and uncommitted peer
  visibility.
- Run direct ownerless `commit-race`, `active-reader-pressure`, and
  `prepared-committed-read` selectors.
- Run the reduced stats-enabled 2048-row production probe and compare against
  `build/manual-ownerless-2048-row-visible-fast-batch.log`.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced stats-enabled production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=20480
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and preserved the 2048-row append-batch shape:

- page-log append calls: `11.900` per statement;
- page versions: `2.000` per statement;
- native-support published pages: `2.000` per statement;
- native-support elided pages: `1846.900` per statement;
- remaining default-checked bulk starts: `0.000` per statement;
- remaining undo-report calls: `2048.000` per statement.

Measured remaining-statement deltas against the preceding 2048-row baseline:

- ownerless/ordinary rows ratio: `0.3195` to `0.3347`;
- row insert: `55.134 ms` to `51.364 ms` per statement;
- clustered insert low path: `51.638 ms` to `48.335 ms` per statement;
- clustered optimistic insert: `34.713 ms` to `31.710 ms` per statement;
- undo report: `25.242 ms` to `23.267 ms` per statement;
- undo-report MTR commit: `16.498 ms` to `14.724 ms` per statement.

## Acceptance Criteria

- Exact modified/dirty page membership uses the cache after the transaction has
  enough pages for linear scans to matter.
- Existing vectors remain authoritative for collection and cleanup.
- Gate erasure cannot leave stale positive cache entries.
- The 2048-row focused selector and adjacent ownerless concurrency selectors
  remain green.
- Reduced production attribution shows the append-batch shape is preserved and
  the remaining later-statement bottleneck is still row-level undo work.

## Risks And Follow-Up

- Cache memory grows with large ownerless transactions. The set is lazy and
  thresholded, but broad DDL/DML shapes need separate pressure evidence.
- Predicate scans over all tracked pages, such as collection and
  space-filtered checks, still use the authoritative vectors.
- The larger write-performance target remains later-statement native
  row-insert/undo-report cost while preserving rollback-to-statement-start and
  savepoint semantics.
