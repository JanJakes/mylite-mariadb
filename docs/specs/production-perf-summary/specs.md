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
  configure, build, CTest, ownerless SQL, and the embedded performance probe.
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
  - when detailed ownerless stats are enabled, ownerless autocommit per-insert
    summaries for page-version volume, native-support page ratio,
    native-support elision volume, page-publish hook/append/index time,
    page-log append time, page-write refresh/publish time, commit-MTR publish
    time, InnoDB write-history time, ownerless visibility time, row-insert
    time, and clustered optimistic B-tree time.
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
already production-build based for timing-sensitive jobs. The slice documents
that audit and strengthens the probes that run under those production builds.

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

The embedded job keeps the default stats-off performance probe as the
throughput signal and runs a second reduced
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` attribution probe so CI logs also
include the ownerless autocommit phase summaries without conflating them with
the stats-off throughput sample.

A later ownerless stress audit found that the checksum-oracle stress can hide
native prepared-statement/update stalls behind the fixed MyLite
`mylite-statements.lock` polling window. Ownerless statement locks now honor a
successful session `SET lock_wait_timeout = N` on that handle, while sessions
that do not set the variable keep the previous 60 second wait. The observed
native prepare/update stall and any finer-grained statement-lock policy remain
a separate performance slice.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build the production embedded performance probe with
  `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`.
- Run a reduced production embedded performance probe and confirm
  `mylite_perf_summary_*` lines are printed.
- Run a reduced production WordPress mysqli `perf-probe` after database
  preparation and confirm `wordpress_perf_summary_*` lines are printed.
- Run focused ownerless primitive CTest coverage under `php-embedded-prod`.
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
transaction-system pages, with `0` published space-metadata pages. The elided
native-support split was `100` undo pages, `38` space-metadata pages, and
`19` transaction-system pages. Summary keys reported `2.000` published
native-support pages per insert and `1.570` elided native-support pages per
insert. This keeps the current performance conclusion focused on
history-related native-support publication plus native InnoDB commit and row
insert internals.

## Acceptance Criteria

- CI and local production probes emit compact summary keys for startup,
  reconnect, read throughput, and write throughput.
- Existing detailed metric keys remain unchanged.
- Stats-enabled ownerless autocommit probes emit per-insert phase summaries
  derived from existing detailed counters.
- CI timing-sensitive jobs remain production-build based and test-only
  WordPress PHPUnit steps remain separated from build/setup phases.
- CI WordPress timing phases require the transient MyLite test database
  directory outside the repository worktree and print the DB parent filesystem
  type.
- CI WordPress mysqli perf-probe and PHPUnit timing phases require the prepared
  database artifacts before they start measuring.
- CI process-isolated WordPress PHPUnit logs include per-child average timing
  keys in addition to total child-process counters.
- CI separates the embedded stats-off throughput probe from the reduced
  stats-enabled ownerless attribution probe.
- CI rejects non-Release CMake caches before CMake-backed test or timing
  phases run.
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
