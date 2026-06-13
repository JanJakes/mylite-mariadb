# Ownerless Page-Log Visible-Fast Batch

## Problem

Production ownerless write attribution after the foreign-key fast-path cache
still shows the write hot path dominated by page-version publication rather
than process startup, reconnect, or plain reads. A reduced Release probe with
page-publish stats enabled reported ownerless active-runtime reconnect at
`1.242 ms` versus ordinary `1.255 ms`, direct `SELECT 1` at `1.04x` ordinary,
and prepared `SELECT 1` at `0.95x` ordinary. Writes remained slower:
ownerless autocommit insert throughput was `0.44x` ordinary, and bulk insert
rows were `0.26x` ordinary.

The same probe reported three page-log appends per ownerless autocommit insert
and two append sessions per insert:

- `mylite_perf_summary_ownerless_autocommit_page_log_append_calls_per_insert=3.017`;
- `mylite_perf_summary_ownerless_autocommit_page_log_session_begin_calls_per_insert=2.000`;
- `mylite_perf_summary_ownerless_autocommit_page_log_session_end_calls_per_insert=2.000`;
- `mylite_perf_summary_ownerless_autocommit_page_log_append_ms_per_insert=0.078`;
- `mylite_perf_summary_ownerless_autocommit_page_publish_hook_ms_per_insert=0.098`.

The next bounded optimization should reduce append-session lock/header churn
without weakening the page-visible ordering that makes ownerless snapshots and
recovery safe.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  calls `mylite_ownerless_innodb_begin_page_publish_batch()` and
  `mylite_ownerless_innodb_end_page_publish_batch()` around dirty page publish
  work in `mtr_t::ownerless_page_writes_publish()` and the ownerless branch of
  `mtr_t::commit_log()`. A pure autocommit `INSERT ... VALUES` can therefore
  open more than one append session while one SQL statement is committing.
- `packages/libmylite/src/database.cc`
  implements those batch hooks with a thread-local
  `mylite_ownerless_page_log_append_session`. A session holds the page-log
  append byte-range lock, reuses the current end offset, and appends records
  through `mylite_ownerless_page_log_append_session_append()`.
- `packages/libmylite/src/ownerless_page_log.cc`
  `mylite_ownerless_page_log_append_session_begin_initialized_at()` acquires
  the append lock, reads the current file identity, reads the page-log header,
  and snapshots the current file end. `mylite_ownerless_page_log_append_session_end()`
  releases the append lock.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_pages_visible_hook()` syncs the page-version WAL before
  publishing the page-visible LSN into shared redo state. Any wider batching
  must release the append session before this sync so visible LSN publication
  never outruns durable page-version records.
- `packages/libmylite/src/database.cc`
  `OwnerlessStatementVisibleFastPathScope` enables visible-fast commit
  visibility for statement shapes already proven by
  `ownerless_statement_allows_visible_fast_path()`, currently pure
  `INSERT ... VALUES` without target-table foreign keys. Append-session
  deferral is narrower and uses a separate MyLite-owned thread-local flag for
  single-row `INSERT ... VALUES` only, because the first implementation sample
  showed that holding the append lock through larger multi-row statements
  regressed bulk insert timing.

## Design

Defer the ownerless page-log append-session release across mini-transactions
only while the new single-row visible-fast append-batch flag is active.

The batch end hook changes from unconditional release to:

1. no-op if no page-log append session is active;
2. keep the session active when the MyLite-owned
   `ownerless_statement_defers_page_log_append_batch` flag is true;
3. otherwise release the session exactly as before.

The page-visible hook calls a local release helper before
`sync_ownerless_page_log_if_changed()`. This preserves the existing durable
ordering:

1. dirty page images are appended to the page-version WAL;
2. any deferred append session releases the append lock;
3. the WAL is synced if its end offset or generation changed;
4. the shared page-visible LSN is advanced.

The statement visible-fast-path RAII scope sets both the MariaDB-visible commit
fast-path flag and the narrower MyLite append-batch flag. Its destructor also
releases any active deferred session before restoring the previous flags,
covering error exits that do not reach page-visible publication. Runtime hook
cleanup releases the current thread's active append session before clearing the
hook context.

## Scope And Non-Goals

In scope:

- single-row ownerless visible-fast-path `INSERT ... VALUES` statements;
- append-session lifetime only;
- focused stats coverage proving session begin/end counts drop while append
  count and existing visibility proof counters remain valid;
- production performance probe evidence.

Out of scope:

- multi-row `INSERT ... VALUES`, broader DML, `INSERT ... SELECT`, upsert,
  `RETURNING`, DDL, explicit transaction, or foreign-key fast paths;
- changing page-version WAL record format;
- changing page-visible LSN publication semantics;
- group commit across independent SQL statements or processes.

## Compatibility Impact

No public SQL, C API, PHP API, mysqli, wire-protocol, metadata, or storage
format behavior changes. Unsupported statement shapes and multi-row VALUES
lists keep the existing per-mini-transaction append-session release behavior
even when they still use the broader visible commit fast path.

## Directory And Lifecycle Impact

No new files or directory layout changes. The append session remains
thread-local and process-local. The durable WAL and checkpoint files remain
inside `concurrency/` under the MyLite database directory.

## Native Storage Impact

No native InnoDB page, redo, undo, or checkpoint format changes. The same page
images are appended with the same commit LSNs. The change only holds the
page-log append lock across adjacent mini-transactions of one visible-fast SQL
statement and releases it before visible-LSN publication.

## Build And Performance Impact

The change is in first-party MyLite hook code and focused test code; it should
not require rebuilding the MariaDB embedded archive unless surrounding MariaDB
source is touched. The expected stats-enabled signal for a single-row
visible-fast insert is:

- page-log append calls per insert remain about three;
- session append calls remain equal to appended records;
- session begin/end calls per pure single-row autocommit insert drop from about
  two to about one;
- no increase in page-log direct append calls for the visible-fast path.

The end-to-end throughput impact is expected to be modest but measurable
because encode and payload writes still dominate part of the append cost.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to first run a
  single-row visible-fast insert with page-log append stats enabled and assert
  successful visible-fast insert batching uses one session begin/end while
  appended records still publish through the session.
- Verify the conservative upsert branch still falls back to commit-visibility
  flush and does not claim visible-fast batching.
- Build and run the focused ownerless selector under `php-embedded-prod`.
- Run a reduced stats-enabled production performance probe and compare
  page-log session counts plus write throughput.
- Run adjacent ownerless selectors covering native-support page publication,
  FK fast-path cache invalidation, primitive page-log behavior, and unsafe hook
  crash visibility where relevant.
- Run production build guards, format checks, `tools/check-ci-production-builds`,
  and `git diff --check`.

## Acceptance Criteria

- The visible-fast-path insert selector proves page-log append sessions are
  reused across a single-row statement's ownerless mini-transactions.
- Page-visible publication still releases and syncs the WAL before publishing
  the visible LSN.
- Error and runtime cleanup paths cannot leave a thread-local append session
  active after the statement or runtime exits.
- Production probe output shows reduced session begin/end counts for ownerless
  autocommit inserts without reducing page-version record coverage.

## Risks And Follow-Up

- Holding the append lock across a larger insert statement can increase append
  wait time for another writer or for local page-version reads. The scope is
  deliberately limited to single-row visible-fast-path inserts; multi-row,
  broader group commit, or statement batching needs separate contention
  evidence.
- This does not reduce page encoding, checksum, or payload-write work. If
  session batching has only a small effect, the next performance slice should
  target page-log encode cost or native-support page proof volume.
