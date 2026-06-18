# Production Performance Summary

## Problem

The ownerless concurrency branch now has several performance probes, but their
raw logs are noisy: the embedded probe prints detailed open, SQL, page-publish,
commit, append, scan, handler, and deep InnoDB counters, while the WordPress
mysqli probe prints process-start, connect, and SQL loop timings. CI already
splits the WordPress PHPUnit work into separate build, dependency, database,
performance-probe, and test-only steps, and those steps run production builds,
but the logs still require manual extraction before branch/main comparisons are
useful.

The immediate performance question is whether the branch is slow because of
build/setup visibility, per-process startup/connect cost, ordinary embedded
engine execution, or ownerless-native page publication. This slice adds compact
summary keys to the existing production probes so CI and local audits can
compare the same high-signal numbers without weakening the detailed counters.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` uses `cmake --preset prod`, `cmake --build
  --preset prod`, and `ctest --preset prod` for the normal build matrix.
- `.github/workflows/ci.yml` uses `php-embedded-prod` for embedded CI
  configure, build, CTest, ownerless SQL, and the embedded performance probes.
  The default stats-off and reduced stats-enabled embedded probes run
  immediately after the production embedded build, before embedded correctness
  tests, so their timing summaries are still visible when a later ownerless SQL
  case fails.
- `.github/workflows/ci.yml` sets
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` for the WordPress PHPUnit job,
  and the harness forwards that build type into its CMake configure step.
- `.github/workflows/ci.yml` already separates WordPress source fetch, MyLite
  PHP extension build, WordPress/PHPUnit dependency install, database
  preparation, mysqli performance probe, `Tests_DB`, isolated tests, and the
  remaining non-isolated suite into separate visible CI steps.
- `tools/wordpress-phpunit-mysqli-mylite` defaults
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE` to `Release`, so local runs that do not
  override it still use optimized PHP extension builds.
- `packages/libmylite/tests/embedded_performance_probe.c` already measures
  ordinary and ownerless warm open/close, active-runtime reconnect, direct and
  prepared `SELECT 1`, transactional inserts, and autocommit inserts. When
  stats are enabled it also emits detailed page-publish, page-log append,
  page-log scan, commit-visibility, handler, and deep InnoDB counters.
- `docs/specs/ownerless-page-publish-volume-profile/specs.md` records that a
  reduced stats-enabled ownerless autocommit sample is not dominated by
  repeated page identities: 3203 page-version publishes contained 3198 unique
  `(space_id,page_no,visible_lsn)` fingerprints and only 5 duplicates.
- A follow-up local production prototype that skipped native-support page
  publication under only the existing single-owner/no-pin proof cut the 400-row
  stats-enabled sample from 3203 page publishes to 400 and page-log append work
  from about `82 ms` to `16 ms`, but did not improve throughput. The same
  prototype inflated write-history timing in one stats-enabled run and a
  2000-row stats-off run regressed ownerless autocommit throughput to about
  `168 ops/s` versus the prior baseline around `295 ops/s`. That broad proof
  was rejected. A later bounded slice elides native-support page WAL only for
  one-row autocommit inserts whose support pages are in the transaction's
  rollback-segment tablespace, relying on the existing native history-space
  flush instead of the single-owner proof alone.

## Design

Keep every existing detailed metric key unchanged. Add compact summary keys at
the end of the probes:

- Embedded C API probe:
  - ordinary and ownerless warm open/close average milliseconds,
  - ordinary and ownerless active-runtime reconnect average milliseconds,
  - ownerless overheads for both open/close measurements,
  - ordinary and ownerless direct and prepared `SELECT 1` throughput,
  - ownerless/ordinary read throughput ratios,
  - ordinary and ownerless transactional and autocommit insert throughput,
  - ownerless/ordinary write throughput ratios,
  - when detailed ownerless stats are enabled, ordinary insert transaction and
    autocommit raw deep InnoDB counters, plus ordinary autocommit baselines and
    ownerless-minus-ordinary deltas for commit, write-history, history-list,
    commit-in-memory, ownerless-visibility, row-insert, and clustered
    optimistic B-tree phases,
  - when detailed ownerless stats are enabled, row-insert subphase summaries
    for transaction start, prebuilt handling, row conversion, row-step
    execution, post-processing, row graph, index-entry, clustered/secondary
    entry, clustered/secondary low-level insertion, clustered-low search,
    duplicate-check, modify-record, instant-root, row-level MTR commit, and
    big-record follow-up phases, and clustered pessimistic B-tree insertion,
  - when detailed ownerless stats are enabled, clustered optimistic B-tree
    summaries for preflight, lock/undo, tuple insertion, reorganization,
    adaptive-hash update, lock update, success and fallback/error counts,
  - when detailed ownerless stats are enabled, B-tree lock/undo summaries for
    setup, lock checking, predicate/record lock checks, undo reporting,
    system-field writes, skip counts, success counts, and error counts,
  - when detailed ownerless stats are enabled, undo-report summaries for
    assignment, cached-undo reuse, fresh undo creation, undo-record page
    reporting, undo-report mini-transaction commit, bookkeeping, page
    extension, and error classes,
  - when detailed ownerless stats are enabled, ownerless autocommit per-insert
    summaries for MTR-published page-version volume, total MyLite
    page-publish hook calls, page-log append calls, transaction-image,
    transaction-buffer, dirty-scan, and buffer-pool-scan page-publish sources,
    native-support page ratio, native-support elision volume, published/elided
    `FIL_PAGE_TYPE_SYS` versus `FIL_PAGE_TYPE_TRX_SYS` splits for the old
    `trx_system` bucket, canonical TRX_SYS byte-diff counters, page-publish
    hook/append/index time, page-log append time, page-write refresh/publish
    time, commit-MTR publish time, InnoDB write-history time, ownerless
    visibility time, row-insert time, and clustered optimistic B-tree time. The
    total hook and append-call summaries are intentionally separate from the
    MTR counters because commit-visible dirty-page publication can reach the
    MyLite page-log path without incrementing the narrower MTR publish counters.
- WordPress mysqli probe:
  - stock PHP process startup,
  - PHP process startup with MyLite extensions loaded,
  - derived extension-load process overhead,
  - PHP process plus mysqli connect/close,
  - derived process/connect delta,
  - in-process mysqli connect/close,
  - active-runtime reconnect,
  - steady `SELECT 1`, transactional insert, point select, prepared autocommit
    insert, and direct autocommit insert throughput,
  - direct/prepared autocommit insert throughput ratio.
- WordPress PHPUnit phase:
  - shell real/user/sys seconds from the existing harness `time` output,
  - PHPUnit-reported test body seconds parsed from the final `Time:` line,
  - shell overhead seconds, computed as shell real time minus PHPUnit-reported
    time, so CI logs separate WordPress/PHPUnit bootstrap/install work from
    the test body.

Use `mylite_perf_summary_*` and `wordpress_perf_summary_*` prefixes so CI log
scraping can distinguish high-signal summaries from detailed phase counters.
The summary values are derived from the same measured intervals that the probes
already print.

Do not change the CI build matrix in this slice because the current workflow is
already production-build based for timing-sensitive jobs. Keep embedded
performance probes before embedded correctness tests so CI produces throughput
and attribution timings even if a later SQL correctness case is red. The slice
documents that audit and strengthens the probes that run under those
production builds.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, metadata, or directory-lifecycle
behavior changes. The slice changes diagnostics emitted by test/performance
harnesses only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The embedded performance probe continues
to create and remove its temporary MyLite directory. The WordPress probe
continues to reuse the prepared WordPress MyLite database directory and drops
its probe tables before closing the connection.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage format changes. The original broad native-support
page-publication suppression was rejected. The later bounded elision relies
only on support pages in `rseg->space->id` that are covered by the mandatory
native history-space flush.

## Wire-Protocol Or Integration-Package Impact

The WordPress mysqli harness prints additional summary lines. The mysqli PHP
extension behavior is unchanged.

## Build And Performance Impact

The probes do a few arithmetic operations and `printf()` calls after existing
measurements. The cost is outside the measured SQL loops and is negligible
compared with process startup, embedded open/close, and database work.

CI timing remains production-build based:

- the MariaDB embedded archive uses the documented production
  `MinSizeRel` baseline,
- first-party matrix jobs use the `prod` preset,
- embedded ownerless, embedded performance, and ownerless attribution jobs use
  `php-embedded-prod`,
- WordPress PHP extensions use `CMAKE_BUILD_TYPE=Release` in
  `build/wordpress-php-embedded-prod`,
- clang-format and clang-tidy configure through the `prod` preset and run the
  production check targets.

CI now also runs `tools/require-cmake-release-build` against generated MyLite
CMake caches and `tools/require-cmake-build-type MinSizeRel` against generated
MariaDB embedded archive caches, so timing-sensitive steps fail early if a
workflow edit or reused build directory stops producing production artifacts.
The workflow audit now checks the timing step bodies themselves, so embedded
test/probe, WordPress dependency/database/perf/PHPUnit, and clang-tool steps
cannot satisfy the audit by leaving production guard strings elsewhere in the
workflow.
The audit also requires the embedded performance and attribution probes to stay
ahead of the embedded test steps. This keeps the branch's production engine
timings visible during performance work even when the ownerless SQL suite finds
a late correctness regression.
The WordPress timing job also enables
`MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1`, which rejects an in-repository
test database path for CI timing phases. The harness prints
`wordpress_db_parent_filesystem_type` so local and CI logs show whether the
WordPress MyLite database was placed on the same storage class as the baseline.
The WordPress mysqli `perf-probe` phase now also requires the prepared
WordPress test config and MyLite database directory before measuring, matching
the `phpunit` phase boundary and preventing skipped setup from being folded
into timing samples.
Default local WordPress PHPUnit runs now match CI's fast process-isolated child
mode by leaving parent child-process profiling and defensive static `wpdb`
scanning disabled. Diagnostic runs can still enable those costs explicitly with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` or
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=1`.
The process-isolated CI shards keep
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` and
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0`, while enabling the lightweight
child timing summary. CI timing summaries include child count, parent
lock-release time, optional per-child baseline-restore time, child runtime,
reconnect time, and per-child averages without enabling the slower reflection
scan or child-body profiling. The database suite and non-isolated suite keep
the global disabled default.
The factory-heavy deferred-reconnect shard now enables
`MYLITE_WORDPRESS_PHPUNIT_CHILD_RESTORE_BASELINE=1` together with child
skip-install. The parent closes WordPress/MyLite handles, restores the prepared
MyLite baseline database directory, and then starts the PHPUnit child with
`WP_TESTS_SKIP_INSTALL=1`. This keeps factory sequence state fresh for
`Tests_Admin_ExportWp` and the process-isolated Sitemaps methods without
running WordPress `install.php` in every child.
The default CI WordPress PHPUnit path also leaves harness-owned JUnit logging
disabled so the split test-only step timings stay comparable to trunk; local
diagnostic runs can still opt into slowest-class and slowest-method reporting
with `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. CI additionally sets
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1`, which adds PHPUnit `--no-logging`
when no explicit logging argument is present. That keeps WordPress'
`phpunit.xml.dist` JUnit logger out of the default timing path; diagnostic
JUnit runs must set `MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=0`.

The prepared DML reset fast path removes the measured server-side
`mysql_stmt_reset()` cost after successful no-result statements. MariaDB's
result-bearing, failed, and metadata-retaining statement reset behavior remains
the authority for those cases. The reduced production attribution sample before
the change reported 500 ownerless autocommit resets taking `120.489 ms` total
with `120.397 ms` in `mysql_stmt_reset()`. The post-change sample reported
500 ownerless autocommit resets taking `0.064 ms` total with `0.000 ms` in
`mysql_stmt_reset()`.

The autocommit no-op fast path keeps the existing exact native-control
classifier but skips non-ownerless `SET autocommit = 0|1` only when
`MYSQL::server_status` already reports the requested state. The PHP mysqli
profile and WordPress timing-summary extraction now expose
`libmylite_exec_result_native_control_autocommit_noops`. A focused production
WordPress `^Tests_DB` sample passed 651 tests with 3 skips and reported
`libmylite_exec_result_native_control_calls=1310`,
`libmylite_exec_result_native_control_autocommit_noops=644`,
`libmylite_exec_result_native_control_ms_total=742.014`,
`query_ms_total=5207.110`, and `wordpress_phpunit_reported_seconds=7.798`.
The previous native-control fast-path sample had the same 1310 native-control
call volume with `libmylite_exec_result_native_control_ms_total=1404.955`.

The embedded job keeps the default stats-off performance probe as the
throughput signal and runs a second reduced
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` attribution probe so CI logs also
include the ownerless autocommit phase summaries and ordinary-versus-ownerless
deep InnoDB deltas without conflating them with the stats-off throughput
sample. The attribution summaries now include row-insert subphase deltas,
including clustered-low search, duplicate-check, row-level MTR commit, and
rare fallback paths, which keeps the next optimization choice tied to measured
InnoDB row graph and B-tree cost instead of the broad
`row_insert_for_mysql()` total.
The `e6efccff` production CI run reported a `0.249 ms/insert`
ownerless-minus-ordinary clustered-low delta, with `0.116 ms/insert` in
clustered optimistic B-tree insertion, `0.129 ms/insert` in row-level MTR
commit, `0.004 ms/insert` in `btr_pcur_open()` search, and zero
duplicate-check, rare fallback, or pessimistic B-tree deltas. A follow-up
optimistic B-tree attribution split now separates preflight, lock/undo, tuple
insert, reorganization, adaptive-hash update, lock update, and result counts.
The final reduced local production sample with that split reported a
`0.480 ms/insert` ownerless-minus-ordinary clustered optimistic B-tree delta,
with `0.476 ms/insert` in `btr_cur_ins_lock_and_undo()`, `0.002 ms/insert`
in tuple insertion, `0.001 ms/insert` in preflight, zero
reorg/adaptive-hash/lock-update deltas, one successful optimistic insert per
row, and no fallback/error counts.
The first reduced local production sample after splitting
`btr_cur_ins_lock_and_undo()` reported a `0.158 ms/insert`
ownerless-minus-ordinary optimistic lock/undo delta, with `0.156 ms/insert` in
`trx_undo_report_row_operation()`, `0.002 ms/insert` in record lock checking,
zero setup/system-field-write deltas, one primary-leaf success per row, and no
skip/error counts.
The undo-report attribution split now separates prelude, persistent/temp undo
assignment, cached-undo reuse, fresh undo creation, insert/update page
reporting, undo-report mini-transaction commit, success bookkeeping, page
extension, and error classes. A reduced 100-row local production sample
reported a `0.103 ms/insert` ownerless-minus-ordinary undo-report delta, with
`0.113 ms/insert` in the undo-report MTR commit bucket, near-zero page-report
encoding and bookkeeping deltas, and no assign, space, record-size, or other
errors. The same sample showed ownerless autocommit cached-undo hits at
`0.810` per insert and fresh undo creates at `0.190` per insert, so the next
performance target is ownerless page publication during mini-transaction
commit rather than SQL-level row encoding or PHP/PHPUnit startup.

The ownerless attribution probe now preserves the historical
`native_support_*_type_trx_system` aggregate while also exposing
`native_support_*_type_sys` and `native_support_*_type_trx_sys`. This avoids
mistaking a generic InnoDB `FIL_PAGE_TYPE_SYS` page for the canonical
`FIL_PAGE_TYPE_TRX_SYS` transaction-system page when choosing the next
performance target. Canonical TRX_SYS byte-diff summaries remain stats-only
and are zero when no `(TRX_SYS_SPACE, TRX_SYS_PAGE_NO,
FIL_PAGE_TYPE_TRX_SYS)` image is published in the measured phase.

A later ownerless stress audit found that the checksum-oracle stress can hide
native prepared-statement/update stalls behind the fixed MyLite
`mylite-statements.lock` polling window. Ownerless statement locks now honor a
successful session `SET lock_wait_timeout = N` on that handle, while sessions
that do not set the variable keep the previous 60 second wait. The observed
native prepare/update stall and any finer-grained statement-lock policy remain
a separate performance slice.

The ownerless SQL harness now retries success-oriented `mylite_open()` calls
for a bounded `MYLITE_BUSY` window. Explicit busy assertions still call the raw
open result helper. This keeps live-peer reclaim and scheduling cases focused
on their steady-state concurrency invariants instead of failing on transient
ownerless startup or process-registry contention between independent test
processes.
The live-idle native-support proof case now matches the runtime safety gate:
while a peer remains live, native-support proof records stay in the page-version
WAL because the native checkpoint proof is process-local; after the final live
peer closes, no-live reclaim must checkpoint that WAL.
The active-reader pressure-limit write case now proves the pressure policy
directly: direct and prepared writes return `MYLITE_BUSY` while a reader pin
retains the WAL at the configured limit, those prepared writes can be retried
successfully after the reader exits, and final close reclaims the retained WAL.
It no longer depends on timer-driven checkpointing before the retry; timer
behavior remains covered by the dedicated idle-runtime scheduling cases.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build the production embedded performance probe with
  `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`.
- Run a reduced production embedded performance probe and confirm
  `mylite_perf_summary_*` lines are printed, including the ownerless
  native-support `SYS`/`TRX_SYS` split when stats are enabled.
- Run a reduced production WordPress mysqli `perf-probe` after database
  preparation and confirm `wordpress_perf_summary_*` lines are printed.
- Run focused ownerless primitive CTest coverage under `php-embedded-prod`.
- Run `tools/check-ci-production-builds` and the production CTest wrapper for
  that audit after workflow edits.
- Run the ownerless cross-process SQL case loop around any changed harness
  expectations.
- Run `cmake --build --preset format`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-08 used the production
`build/php-embedded-prod` and `build/wordpress-php-embedded-prod` artifacts.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- A reduced production embedded performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=20`,
  and `MYLITE_PERF_INSERT_ITERATIONS=20` passed and printed
  `mylite_perf_summary_*` lines, including warm open/close, active-runtime
  reconnect, direct/prepared read throughput, transactional insert throughput,
  and autocommit insert throughput summaries.
- A reduced production WordPress mysqli `perf-probe` with the pinned CI
  WordPress ref `6ddfc9d9b532c6e95c1266165149815895e2eb56`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, one process/connect iteration,
  five SQL iterations, and two write iterations passed and printed
  `wordpress_perf_summary_*` lines, including process startup, process/connect
  delta, in-process connect, active-runtime reconnect, and SQL loop throughput
  summaries.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-primitives$' --output-on-failure` passed.
- `cmake --build --preset format` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed again after formatting.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

A follow-up CI guard slice verified that
`tools/require-cmake-release-build` accepts local `build/prod`,
`build/php-embedded-prod`, and `build/wordpress-php-embedded-prod` Release
caches. A later embedded-archive guard verified that
`tools/require-cmake-build-type MinSizeRel` accepts local
`build/mariadb-embedded` and `build/wordpress-mariadb-embedded` caches and
rejects a temporary Debug cache, keeping the production timing documentation
aligned with the workflow.

Local verification on 2026-06-10 rebuilt the MariaDB embedded archive with the
production `MinSizeRel` cache and rebuilt
`mylite_embedded_performance_probe` plus
`mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`. A reduced
stats-enabled attribution probe with `100` ownerless autocommit inserts printed
`1.000` published `FIL_PAGE_TYPE_SYS` page per insert, `0.000` published
`FIL_PAGE_TYPE_TRX_SYS` pages per insert, `0.190` elided
`FIL_PAGE_TYPE_SYS` pages per insert, and `0.000` canonical TRX_SYS byte-diff
samples per insert. The old aggregate still reported `1.000` published
`trx_system` bucket page per insert, proving that the measured hot aggregate
record in this sample is `FIL_PAGE_TYPE_SYS`, not TRX_SYS.
The matching default stats-off production probe reported ownerless autocommit
at `977.08 ops/s` versus ordinary autocommit at `1537.18 ops/s`, ownerless
transactional inserts at `1243.59 ops/s` versus ordinary transactional inserts
at `1751.70 ops/s`, ordinary active-runtime reconnect at `0.743 ms`, and
ownerless active-runtime reconnect at `0.787 ms`.

The local stats-off production probe after the prepared DML reset fast path
reported ownerless autocommit at `1520.62 ops/s` versus ordinary autocommit at
`3756.37 ops/s`, ownerless transactional inserts at `1567.77 ops/s` versus
ordinary transactional inserts at `4039.43 ops/s`, ownerless direct
`SELECT 1` at `3740.93 ops/s`, ownerless prepared `SELECT 1` at
`1421.40 ops/s`, ordinary warm open/close at `342.713 ms`, ownerless warm
open/close at `378.241 ms`, ordinary active-runtime reconnect at `1.895 ms`,
and ownerless active-runtime reconnect at `1.070 ms`. The stats-enabled sample
still reported about `0.243 ms/insert` in page-log append,
`0.326 ms/insert` in commit-MTR page publication, `0.231 ms/insert` in
write-history, and `0.145 ms/insert` in row-level MTR commit, keeping the next
performance target in native ownerless page publication and commit proof.

A follow-up SYS identity attribution sample, also under production
`MinSizeRel`/`Release` artifacts, reported that the measured SYS pages were
all undo-tablespace pages. The reduced `100`-row ownerless autocommit sample
printed `1.000` published SYS pages per insert, `0.200` elided SYS pages per
insert, first published and elided SYS identity `(space_id=1,page_no=41)`,
`1.000` published SYS undo-space pages per insert, and `0.200` elided SYS
undo-space pages per insert. It reported zero SYS pages in the system
tablespace fixed-page classes (`3`, `4`, `6`, `7`), zero other
system-tablespace SYS pages, and zero non-undo other-space SYS pages. The next
performance question is therefore why undo-space SYS identities are still
published on the hot path, not whether the system-tablespace TRX_SYS page can
be blindly elided.

A WordPress timing placement follow-up showed DB location materially affects
the same production artifacts. With `MYLITE_WORDPRESS_DB_DIR` under
`build/`, a focused `Tests_Formatting_Emoji` process-isolated run passed with
PHPUnit `58.153s`, shell real `88.594s`, child runtime `48.516392s`, lock
release `2.577176s`, and reconnect `2.641894s`. With the default external
`/tmp` database path, the same class passed with PHPUnit `19.638s`, shell real
`30.442s`, child runtime `16.705641s`, lock release `1.075203s`, and reconnect
`0.428487s`. The CI-sized mysqli probe showed the same direction: repo-backed
DB process-plus-connect `928.245 ms` and in-process connect/close `979.783 ms`
versus default external DB process-plus-connect `606.114 ms` and in-process
connect/close `437.155 ms`.

A refreshed branch/main WordPress comparison on 2026-06-08 used production
Release PHP extension builds for both sides. The branch `^Tests_DB` run passed
with PHPUnit `14.088s` and shell real `26.040s`; main `4760d512` passed with
PHPUnit `19.842s` and shell real `33.629s`. CI-sized mysqli perf probes showed
the branch remains close to main for ordinary non-ownerless reads and prepared
writes while retaining the direct-string insert fast path:

- branch: process plus connect/close `555.440 ms`, in-process connect/close
  `399.288 ms`, active-runtime reconnect `3.367 ms`, `SELECT 1`
  `288.20 ops/s`, prepared autocommit inserts `402.36 ops/s`, direct
  autocommit inserts `753.87 ops/s`;
- main: process plus connect/close `481.876 ms`, in-process connect/close
  `358.703 ms`, active-runtime reconnect `5.716 ms`, `SELECT 1`
  `289.99 ops/s`, prepared autocommit inserts `384.88 ops/s`, direct
  autocommit inserts `272.91 ops/s`.

The same audit added a narrow WordPress PHPUnit static-property type filter for
process-isolated parent cleanup. A clean focused `Tests_Formatting_Emoji` run
retained `4531` object-capable static properties, skipped `10` typed
non-object properties, and passed with shell real `28.949s`. A follow-up
profile-output slice adds per-child average keys for process-isolated
lock-release, child runtime, and reconnect time so CI logs expose per-process
cost directly. The open-phase probe confirmed ordinary coordination metadata is
not the process-start bottleneck: ordinary warm open/close averaged
`372.983 ms`, dominated by `mysql_server_init()` and `mysql_server_end()`.

A refreshed production timing audit on 2026-06-09 at `ee26b050` confirmed that
CI build caches are production-mode: `build/prod`, `build/php-embedded-prod`,
and `build/wordpress-php-embedded-prod` are `Release`, while
`build/mariadb-embedded` and `build/wordpress-mariadb-embedded` are
`MinSizeRel`. A quiet reduced embedded probe reported ordinary autocommit
inserts `1885.99 ops/s`, ownerless transactional inserts `1419.99 ops/s`, and
ownerless autocommit inserts `414.05 ops/s`; the stats-enabled attribution run
showed ownerless autocommit cost concentrated in native write-history/page
publication work, including `4.570` page versions per insert and
`0.966 ms/insert` in write-history handling. The CI-sized WordPress mysqli
probe remained in the documented ordinary-path range: process plus connect/close
`583.113 ms`, active-runtime reconnect `3.751 ms`, `SELECT 1` `261.98 ops/s`,
transactional inserts `406.81 ops/s`, prepared autocommit inserts
`316.17 ops/s`, and direct autocommit inserts `609.43 ops/s`.

A later mysqli adapter slice on 2026-06-09 added a one-entry link-local cache
for repeated exact result-producing direct `mysqli_query()` statements while
keeping result metadata on the prepared path and clearing the cache before
no-result statements, `CALL`, explicit prepared statements, schema/charset
helpers, reconnect, and close. The final CI-sized production WordPress
`perf-probe` after that change reported process plus connect/close
`604.162 ms`, in-process connect/close `441.568 ms`, active-runtime reconnect
`3.160 ms`, `SELECT 1` `396.98 ops/s`, point selects `224.03 ops/s`,
transactional inserts `398.21 ops/s`, prepared autocommit inserts
`365.60 ops/s`, and direct autocommit inserts `598.22 ops/s`. This improves
the repeated-result read loop without changing the process-startup diagnosis or
ownerless autocommit publication bottleneck.

The same audit ran the production `^Tests_DB` PHPUnit shard as a test-only
phase. It passed `651` tests with PHPUnit `Time: 00:19.337`, while shell real
time was `53.611s`. The reported-time summary makes that distinction parseable
in CI, so a slow test-only step can be attributed to test body time or
WordPress/PHPUnit bootstrap overhead without confusing either with build work.

A follow-up exact history-page flush slice on 2026-06-09 kept those production
build constraints and changed only the ownerless native history flush prefix.
The reduced stats-enabled attribution probe reported `2.000` exact history
flush pages per ownerless autocommit insert, `0.000` exact-flush fallback
rounds per insert, and the same native page mix, one undo log page and one
rollback-segment system page per insert. The history-flush timing samples were
noisy: `0.669 ms/insert` in the first reduced attribution run, then
`2.566 ms/insert` and `1.297 ms/insert` in final post-relink reruns under
current local load. Three stats-off 2000-row production samples reported
ownerless autocommit throughput of `419.55`, `350.36`, and `391.25 ops/s`;
these remain noisy and do not justify claiming ownerless autocommit is close to
ordinary autocommit yet. The stable outcome is exact native-history page
coverage with the old space-wide wait retained as fallback, not a solved
ownerless autocommit throughput gap.

A follow-up subphase profile on 2026-06-09 kept the same production build
constraints and changed only stats-enabled attribution counters. The post-format
50-row production attribution sample again reported `2.000` exact history
flush pages per insert and `0.000` fallback rounds per insert. The new
subphase split attributed that sample to `0.227 ms/insert` in exact page try
time, `0.736 ms/insert` in exact-write AIO wait time, `0.000 ms/insert` in
fallback time, and `0.004 ms/insert` in final redo-log write time. A separate
stats-off 1000-row production sample reported ownerless autocommit at
`446.88 ops/s`, still in the previously observed noisy branch range.

A refreshed production audit on 2026-06-10 after the transaction-page publish
dedup slice kept all timing inputs on guarded production build directories:
`build/php-embedded-prod` and `build/wordpress-php-embedded-prod` were
`Release`, and `build/mariadb-embedded` plus
`build/wordpress-mariadb-embedded` were `MinSizeRel`. The stats-off embedded
C API sample reported ordinary active-runtime reconnect `1.222 ms`, ownerless
active-runtime reconnect `0.971 ms`, ordinary prepared `SELECT 1`
`2466.47 ops/s`, ownerless prepared `SELECT 1` `2154.50 ops/s`, ordinary
autocommit inserts `1190.87 ops/s`, and ownerless autocommit inserts
`767.28 ops/s`. The reduced stats-enabled attribution sample reported
ownerless autocommit `1166.02 ops/s` versus ordinary `2146.28 ops/s`, with
`3.000` page-version records per insert, `3.570` native-support pages per
insert, `1.570` native-support elided pages per insert, page-log append
`0.080 ms/insert`, commit-MTR publish `0.115 ms/insert`, write-history
`0.108 ms/insert`, row insert `0.153 ms/insert`, and clustered optimistic
B-tree insert `0.070 ms/insert`.

A follow-up production classification probe on 2026-06-10 kept existing
`snapshot_boundary` keys for compatibility but added clearer
non-native-support and actual-boundary keys. The reduced stats-enabled sample
reported ownerless autocommit at `928.62 ops/s` versus ordinary autocommit at
`1756.67 ops/s`, with `3.000` page-version records per insert, `1.000`
non-native-support page per insert, the legacy `snapshot_boundary` value also
at `1.000` per insert, and `0.000` actual synthesized snapshot-boundary pages
per insert. The remaining unpinned autocommit publication cost is therefore
normal user/index page publication plus native-support publication, not active
snapshot-boundary synthesis.

The same production WordPress mysqli probe used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, a prepared test database under
host `/tmp` mounted as tmpfs, Release PHP extensions, and CI-sized iteration
counts. It reported stock PHP startup `55.655 ms`, MyLite-extension PHP startup
`75.788 ms`, process plus MyLite connect/close `603.801 ms`, in-process
connect/close `393.118 ms`, active-runtime reconnect `3.427 ms`, `SELECT 1`
`380.95 ops/s`, point selects `228.71 ops/s`, transactional inserts
`385.62 ops/s`, prepared autocommit inserts `315.41 ops/s`, and direct-string
autocommit inserts `722.09 ops/s`. The production `^Tests_DB` test-only phase
passed `651` tests with `3` skips and reported PHPUnit `Time: 00:15.963`,
shell real `27.582s`, and `wordpress_phpunit_seconds=27`. The current
performance conclusion is unchanged: ordinary WordPress/PHPUnit work is not
regressed on this branch, while remaining ownerless autocommit work is in
page-version/native-support publication plus native InnoDB commit and row
insert internals.

A follow-up production attribution slice on 2026-06-10 split native-support
page publication into published and elided page classes. The reduced
stats-enabled sample reported `457` page-publish candidates, `300` published
page-version records, `357` native-support candidates, `157` native-support
elisions, and `200` published native-support pages. The published
native-support pages were exactly `100` undo pages and `100`
transaction-system-bucket pages, with `0` published space-metadata pages. The
elided native-support split was `100` undo pages, `38` space-metadata pages,
and `19` transaction-system-bucket pages. Summary keys reported `2.000` published
native-support pages per insert and `1.570` elided native-support pages per
insert. This keeps the current performance conclusion focused on
history-related native-support publication plus native InnoDB commit and row
insert internals. A later split proved this transaction-system bucket was not
canonical TRX_SYS in the reduced autocommit sample.

A WordPress PHPUnit partition audit on 2026-06-10 found that the long
non-isolated remaining shard still matched `Tests_DB_Charset`,
`Tests_DB_dbDelta`, and `Tests_DB_RealEscape`, because its negative lookahead
excluded only an exact `Tests_DB` class while the database shard intentionally
runs the whole `^Tests_DB` class family. The remaining-shard filter now starts
with `^(?!Tests_DB)`, so database-prefix tests are timed only in the dedicated
database step. The production-build audit requires that prefix exclusion before
CI can publish WordPress PHPUnit timing logs.

The same audit cycle found that running independent MariaDB embedded CTest
processes with workflow-level `--parallel 2` can produce startup/shutdown
interference unrelated to the test under inspection. Embedded non-ownerless CI
coverage now runs serially under the same Release/MinSizeRel guards, and the
workflow audit rejects future `ctest --parallel N` commands. This trades some
embedded job wall time for stable production timing evidence.

A follow-up production attribution slice on 2026-06-10 split total MyLite
page-publish hook calls and page-log append calls from the narrower
MTR-published page-version counter. A short 100-row production attribution
sample reported `3.000` MTR-published page versions per insert and only
`0.030` extra hook calls per insert, but a serial 1000-row production
attribution sample after cached-undo reuse became hot reported `3.008`
MTR-published page versions per insert, `5.470` total page-publish hook calls
per insert, `2.462` extra hook calls per insert, and `5.470` page-log append
calls per insert. The probe now splits transaction-image, transaction-buffer,
dirty-scan, and buffer-pool-scan page-publish sources so the next performance
slice can target the real non-MTR source rather than guessing from aggregate
hook counts.

The source split showed the extra records came from timer-driven buffer-pool
scan publication, not transaction-page commit visibility: the pre-fix 1000-row
sample reported `0.955` buffer-pool scan publishes per insert and near-zero
transaction-image/transaction-buffer publishes. The timer scheduler now
requires one quiet scheduler interval after the last ownerless statement
activity before reclaiming. The matching 1000-row production attribution
sample reported `3.011` page-publish hook calls per insert, `0.003` extra hook
calls per insert, `3.010` page-log append calls per insert, `0.000`
buffer-pool scan publishes per insert, and ownerless autocommit at
`1176.94 ops/s` versus ordinary autocommit at `1894.09 ops/s`. The matching
stats-off 2000-row production throughput sample reported ownerless autocommit
at `890.53 ops/s` versus ordinary autocommit at `2077.87 ops/s`, compared with
the pre-fix 2000-row ownerless autocommit sample at `284.00 ops/s`.

The completed production CI run for `64e48a6e` kept all timing-sensitive jobs
on production builds and reported `707.17` ownerless autocommit ops/s versus
`2568.21` ordinary autocommit ops/s in the default embedded probe. The
stats-enabled attribution probe reported `3.020` page-log append calls per
insert, `49672.960` page-log bytes per insert, `0.000` buffer-pool scan
publishes per insert, `1.000` published undo native-support page per insert,
and `1.000` published transaction-system-bucket native-support page per
insert. The later `FIL_PAGE_TYPE_SYS`/`FIL_PAGE_TYPE_TRX_SYS` split corrected
that interpretation: in the reduced autocommit sample the bucket is generic
`FIL_PAGE_TYPE_SYS`, so the next bounded performance target is that system page
class rather than blind TRX_SYS elision. Undo history-proof elision remains out
of scope until broader native recovery proof exists.

A follow-up history-proof attribution sample, also under guarded production
builds, showed those two remaining published native-support pages are the
current history WAL proof itself. The reduced `100`-row ownerless autocommit
sample reported `1.000` published history-proof rollback-segment page and
`1.000` published history-proof undo page per insert. Those matched the
`1.000` published `FIL_PAGE_TYPE_SYS` page and `1.000` published
`FIL_PAGE_UNDO_LOG` page per insert, and both were counted as blocked from
blind native-support elision by the active history-proof gate. The same sample
reported `3.000` MTR-published page versions per insert, `3.570`
native-support records per insert, `1.570` native-support elisions per insert,
`49672.960` page-log bytes per insert, page-log append at `0.080 ms/insert`,
commit-MTR publish at `0.177 ms/insert`, write-history at `0.278 ms/insert`,
ordinary autocommit at `1884.01 ops/s`, and ownerless autocommit at
`707.05 ops/s`. The matching stats-off sanity run reported ordinary
autocommit at `1919.23 ops/s`, ownerless autocommit at `644.07 ops/s`,
ordinary transactional inserts at `1381.87 ops/s`, ownerless transactional
inserts at `837.62 ops/s`, and ownerless active-runtime reconnect at
`1.217 ms`. The next performance target is therefore replacing or compressing
the history-proof page-version evidence, not blind elision of the current proof
pages.

A follow-up ownerless page-log payload slice keeps those history-proof records
but encodes zero-heavy page images in the page-version WAL record format. The
record header keeps the full page size and either a sparse-zero or trailing-zero
flag, while the stored payload contains a nonzero-run list or nonzero prefix
and the checksum still covers the reconstructed full page image. Tail-only
encoding was effectively neutral in the hot sample (`4947967` payload bytes
versus `4947968` before the slice), because InnoDB page trailers stayed
nonzero. The final sparse encoder reduced the same 100-insert stats-enabled
sample to `796761` page-log payload bytes total, `7967.610` payload bytes per
insert, and `8160.890` total page-log bytes per insert, while page-log append
time stayed near the previous sample (`0.083 ms/insert` versus
`0.085 ms/insert`). Primitive coverage verifies sparse zero-range,
tail-prefix, and zero-byte page payload readback, append-session offset
advancement by encoded payload size, and checkpoint compaction of encoded
retained records. A 500-insert stats-enabled sample showed later table pages
becoming less sparse, with page-log payload at `17375.654` bytes per insert,
page-log append at `0.120 ms/insert`, and ownerless autocommit at
`1034.69 ops/s` versus ordinary autocommit at `2072.58 ops/s`; broader
end-to-end write-throughput claims still need the remaining unaccounted
prepared-step time instrumented.

A follow-up CI-timing visibility and harness-stability audit on 2026-06-11 kept
the same production guards and moved the default embedded performance probe and
reduced ownerless attribution probe ahead of embedded correctness tests. Local
`tools/check-ci-production-builds` and `ctest --preset prod -R
'^tools\.ci-production-builds$' --output-on-failure` passed, proving the
workflow body still rejects non-production caches and now rejects embedded
probe steps placed after embedded correctness tests. The ownerless SQL harness
changes were verified with focused loops for the live-idle native-support proof
case and active-reader pressure-limit case, plus a resumed CI-style case loop
from case `45` through case `168`; together with the preceding case `0`
through `44` pass, this covered the full ownerless SQL case set around the
previous CI failures.

The matching stats-off production embedded probe reported ordinary warm
open/close `385.622 ms`, ownerless warm open/close `371.812 ms`, ordinary
active-runtime reconnect `1.186 ms`, ownerless active-runtime reconnect
`0.752 ms`, ordinary transactional inserts `4343.45 ops/s`, ownerless
transactional inserts `158.44 ops/s`, ordinary autocommit inserts
`3687.35 ops/s`, and ownerless autocommit inserts `182.57 ops/s`. The reduced
stats-enabled attribution probe reported ownerless autocommit
`273.89 ops/s`, `3.000` MTR-published page versions per insert, `3.020`
page-log append calls per insert, `2.000` published native-support pages per
insert, `1.600` elided native-support pages per insert, `0.624 ms/insert` in
write-history, `1.154 ms/insert` in row insert, `0.695 ms/insert` in
clustered optimistic B-tree insertion, and `0.653 ms/insert` in undo-report
MTR commit. This preserves the current performance conclusion: startup and
active-runtime reconnect are visible and not the dominant branch gap, while
ownerless write throughput remains the next optimization target.

The first post-split production CI run for `31f3271d` confirmed the timing
steps were visible before embedded correctness but then failed in ownerless SQL
case `13` (`test_ownerless_independent_table_stress`). Local production
reproduction showed a direct `mylite_exec()` reader could observe a lower
per-table aggregate after seeing a newer page-version state. The follow-up
runtime fix keeps successful direct-read handle pins across statements and
prevents retained reads from replacing user data/index/blob pages with lower
visible-boundary images during visible-boundary external refresh, clean-page
refresh, or file-read overlay while still accepting current ownerless page
images that advance a page; native support pages still refresh normally. The
same `sql-case 13` loop that
reproduced the failure passed `30/30` locally after the fix, and focused
active-pin and active-reader pressure diagnostics remained passing. The loop
still emitted intermittent non-fatal InnoDB undo-page warnings during local
verification, so undo/checkpoint reconciliation remains separate follow-up
evidence rather than a performance-timing conclusion. This keeps the production
timing split useful: correctness failures are still visible, but they no
longer hide whether the job used optimized artifacts.

The first CI run after that fix exposed the narrower current-boundary case in
`sql-case 3` (`test_ownerless_concurrent_transaction_commits`): a verifier
handle that retained an earlier direct read could reject current ownerless page
images for other independent tables when the final read was already using the
live boundary. Retained-read overlay protection therefore distinguishes
current ownerless page images from lower visible-boundary overlays instead of
turning protection off for current reads. The same local production loop then
exposed a second
commit-race invariant: no-live close-time reclaim could advance durable
`mylite-concurrency.ckpt` past the still-existing volatile `.shm` redo-visible
state, leaving `.shm` stale until a rebuild. No-live reclaim now reseeds the
runtime redo state from the durable checkpoint whenever it advances the visible
LSN, keeping the timing-visible production job from failing on a volatile
metadata lag after correctness has already been made durable.

The next production ownerless SQL run exposed `sql-case 46`
(`test_ownerless_active_reader_pressure_limit_blocks_write_classes`) during a
same-runtime direct read after a local AUTO_INCREMENT insert and before later
pressure-guarded DDL. The fix keeps autocommit write visibility in a separate
local-native read boundary. During a continuous single-owner epoch that neither
started with nor consumed page-version WAL, eligible same-runtime reads covered
by that boundary stay on native InnoDB pages and do not publish page-version
pins or enable the file-read overlay. This removes avoidable per-page WAL hook
cost from local verification reads. Autocommit writes outside that strict local
proof still advance the real page-version read LSN, keeping multi-process
same-handle read-your-writes and retained-overlay semantics for cases such as
`sql-case 13`.

Local production verification on 2026-06-11 rebuilt
`mylite_ownerless_cross_process_sql_test` under `php-embedded-prod`, then
passed repeated direct loops for `sql-case 13`, `sql-case 46`, and
`sql-case 3` at `20/20` each with `/tmp` ownerless cleanup between iterations.
The `sql-case 13` loop still printed two non-fatal InnoDB undo-page warnings,
matching the existing undo/checkpoint follow-up risk, but its stress oracles
and final reopen checks passed. Focused ownerless primitive/single-owner CTest
coverage and embedded ownerless hook CTest coverage passed under
`php-embedded-prod`, and `tools/check-ci-production-builds` plus its production
CTest wrapper passed. A reduced production embedded performance probe with
`20` read/write iterations printed the expected summary keys, including
ownerless direct `SELECT 1` at `614.31 ops/s`, ownerless prepared `SELECT 1` at
`363.79 ops/s`, ownerless transactional inserts at `1335.56 ops/s`, and
ownerless autocommit inserts at `1344.07 ops/s`; those small-sample throughput
values are timing smoke evidence, not a replacement for CI-sized samples.

## Acceptance Criteria

- CI and local production probes emit compact summary keys for startup,
  reconnect, read throughput, and write throughput.
- Existing detailed metric keys remain unchanged.
- Stats-enabled ownerless autocommit probes emit per-insert phase summaries
  derived from existing detailed counters, including separate MTR-published
  page-version, total page-publish hook-call, page-log append-call, and
  non-MTR page-publish source rates, plus clustered-low row-insert subphases.
- CI timing-sensitive jobs remain production-build based, and test-only
  WordPress PHPUnit steps remain separated from build/setup phases.
- CI embedded non-ownerless CTest coverage runs serially so production timing
  evidence is not distorted by concurrent MariaDB embedded runtime startup.
- CI WordPress timing phases require the transient MyLite test database
  directory outside the repository worktree and print the DB parent filesystem
  type.
- CI WordPress mysqli perf-probe and PHPUnit timing phases require the prepared
  database artifacts before they start measuring.
- CI process-isolated WordPress PHPUnit logs include per-child average timing
  keys in addition to total child-process counters.
- CI process-isolated WordPress PHPUnit logs include per-child prepared
  database baseline restore count, total time, and average time when the
  baseline-restored child mode is enabled.
- CI's long non-isolated WordPress PHPUnit shard excludes the full
  `Tests_DB*` class family so database-prefix tests are not duplicated between
  the database and remaining test-only steps.
- CI separates the embedded stats-off throughput probe from the reduced
  stats-enabled ownerless attribution probe.
- CI runs both embedded performance probes before embedded correctness tests so
  production timing evidence remains visible even if a later correctness case
  fails.
- Stats-enabled ownerless attribution summaries include native page-write
  publish and commit-log subphase rows so CI logs show whether remaining write
  cost sits in scan, page image preparation, hook/page-log append, redo-leave,
  or no-dirty commit-loop work.
- Ownerless write-path optimization slices use those rows to target native
  InnoDB page-write overhead first; the transaction-release classification fast
  path is a scoped example and does not claim to complete redo/checkpoint or
  history-proof publication work.
- Modified-page entry and commit-publication paths now reuse already-computed
  transaction-held-page or `transaction_publish` decisions when marking pages
  for transaction-deferred dirty publishing. This avoids another redundant
  ownerless classification hop without changing publication volume.
- Ownerless checkpoint update summaries include
  `checkpoint_update_legacy_write_elided_per_insert`, distinguishing legacy
  payload write removal from same-pair no-op elision and file-read elision.
- Ownerless page-log append summaries include append lock/fstat/header/body
  setup, checksum, payload write, and record-header write rows so CI can
  distinguish WAL encoding cost from positioned-write cost.
- CI rejects non-Release CMake caches before CMake-backed test or timing
  phases run.
- Timer-driven ownerless checkpoint scheduling remains an idle-runtime cleanup
  path and does not publish buffer-pool scan page versions during continuous
  ownerless statement activity.
- Docs record that the native-support page-publish skip prototype is not an
  accepted optimization because it failed throughput validation.

## Risks And Unresolved Questions

- Probe summaries make slow paths visible; they do not by themselves reduce
  per-process startup, mysqli connect, or ownerless autocommit cost.
- Reduced local probe runs are noisy. Optimization decisions still require
  repeated production samples with comparable storage placement and runner load;
  the WordPress harness now makes DB placement visible and CI-guarded.
- Broader native redo/checkpoint reconciliation remains the prerequisite before
  ownerless native-support page publication can be safely reduced.
