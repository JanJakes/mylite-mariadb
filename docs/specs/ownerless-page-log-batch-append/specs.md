# Ownerless Page-Log Batch Append

## Problem

The deep ownerless autocommit profile shows the remaining 400-row production
autocommit gap is dominated by `trx_t::write_serialisation_history()` and its
commit mini-transaction. In the same stats-enabled run,
`page_write_commit_log_publish_ms=170.927` and
`page_log_append_total_ms=126.903`; append subphases include
`page_log_append_fstat_ms=21.817`, `page_log_append_payload_write_ms=76.009`,
and `page_log_append_record_header_write_ms=7.919`.

The current ownerless page-version append path takes the page-log append
byte-range lock and re-reads the file size for every page-version record. A
single InnoDB commit mini-transaction can publish multiple page versions, so
the per-record lock and file-size discovery are avoidable hot-path overhead.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_writes_publish()` scans modified MTR memo slots after
  a commit LSN exists and calls `ownerless_page_write_publish()` for each page
  that must be made visible through MyLite page-version WAL.
- `mtr_t::ownerless_page_write_publish()` copies and checksums the final page
  image, then calls `mylite_ownerless_innodb_publish_page_version()`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  dispatches `mylite_ownerless_innodb_publish_page_version()` to the first-party
  MyLite page-publish callback.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_publish_hook()` publishes an optional active-reader
  snapshot-boundary record, pauses at unsafe test hooks, appends the page
  version with `mylite_ownerless_page_log_append_initialized_at()`, then updates
  the shared page-version index.
- `packages/libmylite/src/ownerless_page_log.cc`
  `append_at_common()` acquires the append lock per append call, and
  `append_locked()` uses `fstat()` per record to find the current end of the
  log before writing payload bytes followed by the record header. Writing the
  payload before the header is the record-completion ordering and must not
  change.

## Design

Add an internal initialized append-session API for the ownerless page log:

- begin: acquire the append byte-range lock once, verify the initialized header
  size, read the current file end once, and store the next record offset in a
  small caller-owned session object;
- append: write one payload and then its record header at the session's current
  offset, return that record offset, and advance the session end offset;
- end: release the append lock.

Wire optional page-publish batch begin/end hooks around
`mtr_t::ownerless_page_writes_publish()`. The InnoDB begin hook only marks the
current thread as eligible for batching. The MyLite callback begins the page-log
append session lazily on the first actual page-version append, so empty MTR
publish scans and unsupported/fallback paths do not take the append lock.
`ownerless_innodb_page_publish_hook()` and snapshot-boundary synthesis use the
active session when present, otherwise they keep the existing per-record append
path.

Active-reader snapshot-boundary synthesis can need a page-log read before the
current page image is appended. If a lazy append session is already active, the
callback releases it before the boundary read. This preserves the existing
read-side lock ordering and avoids holding the append byte-range lock while a
boundary scan runs.

Do not enable the batch session when ownerless unsafe test faults are enabled.
Those tests pause before and after page append; holding the append byte-range
lock across those injected pauses would change crash/race timing and could hide
or create test-only deadlocks.

The WAL record format, payload-before-header completion order, page-version
index publication, checkpoint format, recovery replay, and durability semantics
remain unchanged.

## Compatibility Impact

No SQL, public C API, PHP API, wire-protocol, or MySQL/MariaDB compatibility
behavior changes. The new symbols are internal ownerless/InnoDB integration
helpers.

## Directory And Lifecycle Impact

No new files or directory layout changes. The session is process-local and
thread-local; the durable page-version WAL remains
`concurrency/mylite-concurrency.wal`.

## Native Storage Impact

No native InnoDB page, redo, undo, or data dictionary format changes. The slice
only changes how MyLite batches ownerless page-version WAL appends around an
existing InnoDB MTR publication boundary.

## Performance Impact

Stats-off production behavior does fewer append-lock acquisitions and file-size
reads for multi-page MTR publish batches. The optimization does not remove
payload writes, checksums, or page-version index updates, so it reduces only
part of the commit-MTR publication bucket.

Measured production evidence on the reduced probe:

- previous deep profile, 400 ownerless autocommit inserts with stats enabled:
  `page_log_append_fstat_ms=21.817`,
  `page_log_append_total_ms=126.903`, and
  `page_write_commit_log_publish_ms=170.927`;
- this slice, 400 ownerless autocommit inserts with stats enabled:
  `page_log_append_fstat_ms=2.576`,
  `page_log_append_lock_ms=2.113`,
  `page_log_append_total_ms=120.087`, and
  `page_write_commit_log_publish_ms=170.596`;
- this slice, stats-off production samples for 400 ownerless autocommit
  inserts measured `383.87`, `240.06`, and `289.00` ops/s, showing meaningful
  run-to-run noise; a longer 2000-row stats-off production sample measured
  ordinary autocommit at `1886.15` ops/s and ownerless autocommit at
  `294.64` ops/s.

In the batched path, session-begin lock and `fstat()` time is charged to the
lock/fstat subcounters but not to each per-record append total. Use the
commit-MTR publication bucket and the explicit lock/fstat subcounters for
end-to-end comparisons.

The fstat/lock subphase improved as designed, but the ownerless autocommit gap
remains dominated by page-version write volume and native-support page
publication. Further optimization should reduce page-version volume or native
support page publication with stronger redo/checkpoint evidence, not spend more
time on append metadata.

## Test Plan

- Rebuild the MariaDB embedded archive after editing InnoDB hook files.
- Rebuild production embedded targets for the performance probe and focused
  ownerless SQL tests.
- Run reduced stats-off and stats-on production performance probes and compare
  ownerless autocommit throughput plus `page_log_append_fstat_ms`,
  `page_log_append_lock_ms`, and `page_write_commit_log_publish_ms`.
- Run focused ownerless SQL selectors for committed-read visibility, native
  reclaim, live reclaim, commit race, active-reader pressure, and a DDL case
  that exercises page-publish test hooks.
- Run the production ownerless SQL direct-case loop used by CI, with `/tmp`
  ownerless cleanup between cases.
- Run `ownerless-test-hooks` negative proof, `ownerless-stress`,
  production non-ownerless embedded CTest split, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Page-version WAL records remain readable after normal ownerless SQL, forced
  `.shm` rebuild, and native reopen.
- Unsafe hook tests continue to exercise the existing per-record append timing.
- Reduced production ownerless autocommit stats show lower append lock/fstat
  cost or otherwise document why batching did not reduce the measured hot path.
- The diff keeps MariaDB-derived changes narrow and first-party batching logic
  behind MyLite-owned names.

## Risks And Follow-Up

- Holding the append lock for a whole MTR may increase peer append wait time for
  large MTRs. Keep the batch strictly around one MTR publish scan and rely on
  existing stress tests to catch starvation or deadlocks.
- This slice does not reduce page-version volume. If batching helps only
  modestly, the next optimization must reduce published page count or payload
  bytes with stronger native redo/checkpoint evidence.
