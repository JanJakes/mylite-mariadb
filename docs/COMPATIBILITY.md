# Compatibility

MyLite compatibility is tracked by surface area, not by broad claims. MariaDB
11.8 is the primary behavior authority; MySQL behavior is additional evidence
for drop-in application expectations.

## Status Key

- ✅&nbsp;Covered: implemented and covered by committed tests.
- 🟡&nbsp;Partial: implemented with documented limits and committed tests.
- ⚪&nbsp;Planned: target behavior for an upcoming slice.
- ➖&nbsp;Out&nbsp;of&nbsp;scope: deliberately omitted from the embedded
  single-directory product.

## Baseline

| Area | Target |
| --- | --- |
| MariaDB base | MariaDB 11.8 LTS, initial import ref `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7` |
| Runtime shape | Embedded in-process library, no daemon required for core use |
| Durable storage | One MyLite-owned database directory containing MariaDB native storage files and documented MyLite lifecycle metadata |
| Primary API | `libmylite` directory-owned C API |
| MariaDB C API | Optional adapter, not the primary lifetime model |

## Harness

Compatibility coverage is grouped with CTest labels under the `embedded-dev`
preset for local development. CI timing evidence uses production presets and is
guarded separately by `tools/check-ci-production-builds`, `Release` MyLite
cache checks, and `MinSizeRel` MariaDB embedded cache checks.

| Group | Command |
| --- | --- |
| Embedded lifecycle | `ctest --preset embedded-dev -L compat.lifecycle` |
| Directory-boundary detection | `ctest --preset embedded-dev -L compat.directory-boundary` |
| MariaDB-reference SQL results | `ctest --preset embedded-dev -L compat.mariadb-comparison` |
| Crash/reopen behavior | `ctest --preset embedded-dev -L compat.crash-reopen` |
| Concurrency | `ctest --preset embedded-dev -L compat.concurrency` |
| Ownerless primitives | `ctest --preset embedded-dev -L compat.ownerless-primitives` |
| Ownerless directory lifecycle | `ctest --preset embedded-dev -R libmylite.embedded-ownerless-directory-lifecycle` |
| Ownerless product hook binding | `ctest --preset embedded-dev -R libmylite.embedded-ownerless-product-hooks` |
| Ownerless transaction hooks | `ctest --preset embedded-dev -L compat.ownerless-transaction` |
| Ownerless InnoDB lock hooks | `ctest --preset embedded-dev -L compat.ownerless-innodb-lock` |
| Ownerless cross-process SQL | `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`, registered as sixteen deterministic weighted CTest shards with flushed per-case progress diagnostics so long ownerless SQL coverage reports per-shard failures, estimated shard weights, timings, active case names, and active case indexes on timeout; hidden per-case children run in their own process groups so timeout cleanup cannot leave orphaned descendants holding CTest output pipes open; a timed-out case can be rerun directly with `mylite_ownerless_cross_process_sql_test sql-case <index-or-name>` through the same wrapper, `sql-case-count` reports the current direct-case loop bound, and the old modulo `sql-shard <index> <count>` command remains available for comparison; earlier two-job and four-job modulo-shard scheduling produced load-sensitive ownerless DDL/dictionary/temporary-tablespace timeouts, while weighted-shard ownerless SQL measurement passed at two jobs locally with about half the wall time; a later smaller-shard slice split the registered weighted group from eight to sixteen shards for more granular timings without weakening the 300-second per-case watchdog; full-preset two-job scheduling still timed out when ownerless and non-ownerless tests interleaved, so CI runs non-ownerless embedded tests and ownerless SQL as separate visible steps; the non-ownerless step now runs serially with `--output-on-failure` after local reproduction showed independent MariaDB embedded CTest processes can interfere during startup/shutdown, while the ownerless SQL step runs each case through direct `sql-case <index>` with `/tmp` ownerless cleanup between cases after paired, one-shot serial, and isolated-shard ownerless aggregate runs timed out even though the timed-out cases passed directly |
| Ownerless cross-process stress | `cmake --preset ownerless-stress && cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test && ctest --preset ownerless-stress`, covering independent-table, DDL/DML, temporary-table, explicit-transaction, checksum-oracle, pseudo-random shared-table transaction, foreign-key graph, stress child-failure cleanup, active-reader pressure, expanding-page pressure, BLOB page pressure, compressed BLOB page pressure stress cases, deterministic SQL trace exporters including DDL lifecycle with same-name recreate `SPACE` identity plus replacement-copy DDL oracles, CTAS post-create DML, active-reader pressure including AUTO_INCREMENT high-watermark final oracle, and BLOB pressure, full deterministic trace-suite validation, seeded random transaction, DDL stress, and FK graph trace validation, scaled check-mode validation for active-reader and BLOB pressure traces, opt-in Docker-backed external MariaDB deterministic trace replay with focused `--trace` and bounded `--scale` profiles including refreshed full scale-2 deterministic replay evidence for all 11 trace families and focused seeded DDL stress replay evidence, and the external trace-runner fake-client smoke test |
| Ownerless negative proof | `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof` |
| Platform probes | `ctest --preset embedded-dev -L compat.platform`; hook-only ownerless open rejection is covered by `ctest --preset ownerless-test-hooks -R libmylite.ownerless-platform-probe-failure` |
| Application queries | `ctest --preset embedded-dev -L compat.application-query` |
| Engine clauses | `ctest --preset embedded-dev -L compat.engine` |
| Server surfaces | `ctest --preset embedded-dev -L compat.server-surface` |
| Current SQL query surface | `ctest --preset embedded-dev -L compat.query` |

The MariaDB-reference group uses expected result vectors pinned to MariaDB 11.8
behavior. It does not require a daemon in the default test path.

The deterministic ownerless SQL trace suite currently contains 11 trace
families after adding CTAS post-create DML export; the DDL lifecycle trace now
records per-round same-name recreated InnoDB `SPACE` identity oracles in its
generated SQL, and the active-reader pressure trace includes an
AUTO_INCREMENT high-watermark final oracle. Docker-backed MariaDB 11.8 full
scale-2 replay evidence now covers the current 11-family suite including those
oracles, and Docker-backed MariaDB 11.8 random transaction and DDL stress
seed-suite replay covers seeds `0`, `17`, `83`, and `211` at rounds `8`; the
FK graph trace now also has seeded trace-suite and optional Docker-smoke entry
points using the same default seed set, with bounded Docker-backed MariaDB 11.8
replay evidence for seeds `0`, `17`, `83`, and `211` at rounds `2` after
whole-seed retries recovered transient raw MariaDB `1213` escapes for seeds
`17` and `83`.
Dependency-free CTest
check-mode coverage validates the random transaction, DDL, and FK graph seeded
wrappers plus their external wrapper plans with the same seed set, plus the
combined seed-sweep wrapper over seeds `0` through `15` at rounds `3` and a
wider combined seed-sweep wrapper over seeds `0` through `31` at rounds `2`;
a focused seed-sweep replay through one disposable MariaDB 11.8 server covers
the random transaction and DDL seeded suites over seeds `0`
through `7` at rounds `4`, and a follow-up replay covers seeds `8` through
`15` at rounds `2`.
Longer randomized external MariaDB/RQG stress remains planned.

Ownerless stale-reader file-lifecycle replay now includes same-schema and
cross-schema same-statement multi-table `DROP TABLE` coverage for multiple
removed file-per-table tablespaces, rename-away plus new original-name
`CREATE TABLE` coverage that preserves both final file-per-table spaces, and
same-name `CREATE OR REPLACE TABLE` replacement coverage, including `... LIKE`
copied-shape replacement and the `... AS SELECT` populated replacement variant,
in addition to the single-table drop, stale-reader retained-WAL killed-drop,
and multi-table schema-drop evidence tracked below. This is still bounded
replay evidence, not a claim that the
broader durable DDL file-lifecycle protocol is complete.

Ownerless performance diagnostics now run through production build presets for
CI-visible timings, and CI separates the stats-off embedded throughput probe
from a reduced stats-enabled ownerless attribution probe. CI also separates the
WordPress PHPUnit source, build, dependency, database-prep, performance-probe,
and test-only phases so PHPUnit wall timings are not hidden inside build work.
The embedded performance and attribution probes run before embedded correctness
tests, so production throughput and attribution numbers remain visible even
when a later ownerless SQL case fails.
The probe also reports direct multi-row `INSERT ... VALUES` row-list timing
with `mylite_perf_bulk_insert_rows_per_statement`,
ordinary/ownerless bulk row and statement throughput, ownerless/ordinary bulk
ratios, and stats-enabled per-row/per-statement ownerless attribution for page
versions, page-log appends, native-support publication, and commit-visibility
choices. This separates the single-row prepared autocommit cost from the
multi-row fast-path SQL shape that ownerless concurrency now admits.
Ownerless mini-transaction page-write release now skips transaction lookup and
external release policy checks when the current MTR has no ownerless
page-write pages left to release. In the CI-shaped stats-enabled bulk probe,
this reduced ownerless bulk `page_write_leave_total_ms` from `3.892` to
`0.551`, kept bulk commit visibility on the fast path with zero publish
failures, and preserved native latch/memo release ordering.
Ownerless page-version publication now skips synthesized snapshot-boundary
probes for native-support page classes and returns before the page-pin registry
latch when no active pins exist; normal page-version publication, history-proof
native-support records, active-reader retention, and boundary synthesis for
snapshot-sensitive pages remain unchanged. This is a bounded hot-path pruning
slice, while replacing or shrinking the remaining history-proof page
publication remains a planned performance target.
The timing-producing WordPress dependency, database-prep, performance-probe,
and PHPUnit test-only steps repeat `Release` MyLite and `MinSizeRel` MariaDB
embedded cache guards inside the step body, so a stale build directory fails
before it can publish misleading PHPUnit or perf-probe timings.
The WordPress PHPUnit harness now defaults to the same fast process-isolated
child mode used by CI: parent child-process profiling and defensive static
`wpdb` scanning are off unless a diagnostic run explicitly sets
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` or
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=1`.
The slow non-isolated WordPress PHPUnit CI step now enables
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, which sets
`MYLITE_MYSQLI_PROFILE=1` only for that PHPUnit process and emits
`mylite_mysqli_profile_*` open/close, direct-query, result-query,
prepared-statement, cache, result-step, row-materialization,
status-synchronization, result-object, and fetch counters plus fetch elapsed
time from the production-built mysqli adapter. A focused production
`Tests_DB` run after the keepalive slice reported `fetch_object_calls=76626`
but only `fetch_object_ms_total=122.798`, while `query_ms_total=19084.516`,
`exec_no_result_ms_total=6329.991`, `query_result_step_ms_total=4828.470`,
`query_prepare_ms_total=4327.226`, and `query_cache_clear_ms_total=3530.218`
remained much larger. The current WordPress performance target is therefore
MariaDB/libmylite query execution and prepared-statement lifecycle cost, not
PHP fetch-object conversion. Other WordPress CI timing steps keep that profile
disabled unless a diagnostic run explicitly opts in.
The stats-enabled embedded performance probe now also reports ownerless
prepared-DML native `mysql_stmt_prepare()` and `mysql_stmt_close()` call counts
and elapsed time, plus per-insert summaries, so the remaining prepared-write
cost can be separated from startup, reconnect, page-version publication, and
PHP adapter overhead. Ownerless prepared no-result DML now executes eligible
`INSERT`, `UPDATE`, `DELETE`, and `REPLACE` statements without `RETURNING`
through MariaDB's length-aware text query path after rendering bound values as
SQL literals, avoiding per-step native prepare/close while keeping the native
prepared-handle cache blocked until a directory-owned peer-join barrier exists.
The mysqli adapter can preserve prepared result statements
across ordinary no-result `INSERT`, `UPDATE`, `DELETE`, and `REPLACE`
statements without `RETURNING`, while retaining conservative cache clears for
DDL, schema, transaction, lock, `SET`, `USE`, `CALL`, and error paths. The
focused production `Tests_DB` sample after this cache-retention slice reported
`query_cache_preserved_no_result_calls=111`, `query_cache_hits=3`,
`query_prepare_calls=1612`, `query_cache_clear_finalize_calls=1612`,
`query_ms_total=15296.434`, and
`exec_no_result_ms_total=6775.758`, with
`wordpress_phpunit_reported_seconds=19.314`; the previous focused attribution
sample reported `query_cache_hits=0`, `query_prepare_calls=1615`,
`query_cache_clear_finalize_calls=1615`, `query_ms_total=19084.516`, and
`wordpress_phpunit_reported_seconds=26.509`. Full non-isolated shard timings
remain the authority for suite-wide impact.

`mylite_reset()` now skips MariaDB's `mysql_stmt_reset()` only when a prepared
statement has been fully drained to `MYLITE_DONE`; partial results and active
server cursor state still use the conservative MariaDB reset path. The fully
drained path also retains result metadata and bind buffers for reuse on the
same prepared SQL. MariaDB client tests in the imported 11.8.6 source
re-execute prepared statements after `MYSQL_NO_DATA`, and the focused libmylite
prepared-statement test covers both fully-drained result re-execution and
partial-result reset. This fast path is the current mitigation for repeated
mysqli cached result-query cost. A production focused profile over 1000 cached
`mysqli_query('SELECT 1')` calls after the fast path reported
`query_cache_hits=999`, `query_prepare_calls=1`,
`query_cache_lookup_ms_total=0.371`, and `php_select1_ops_per_second=789.52`.
The CI-shaped production WordPress performance probe reported
`select1_ops_per_second=829.69`; the earlier production probe before this
fast path reported about `383.57`.

The diagnostic prepared-result route uses a bounded exact-SQL LRU instead of a
single entry. This keeps prepared result metadata for interleaved repeated
queries while retaining the same conservative invalidation points for DDL,
schema, transaction, lock, `SET`, `USE`, `CALL`, explicit prepared statements,
reconnect, close, and error paths. The profile test covers non-consecutive
exact SELECT reuse in addition to DML-preserved current-row visibility. The
focused production `Tests_DB` profile after this LRU slice reported
`query_cache_hits=13`, `query_cache_misses=1602`,
`query_prepare_calls=1602`, `query_cache_clear_finalize_calls=1602`,
`query_ms_total=11354.274`, and `wordpress_phpunit_reported_seconds=14.692`;
the pre-LRU fast-reset profile reported `query_cache_hits=3`,
`query_cache_misses=1612`, `query_prepare_calls=1612`,
`query_cache_clear_finalize_calls=1612`, `query_ms_total=13141.702`, and
`wordpress_phpunit_reported_seconds=16.442`. The remaining profile still shows
mostly unique result SQL and no-result execution cost rather than cache lookup
overhead.

The default mysqli result-query path now uses libmylite's binary-safe
text-result callback for first-seen and non-repeated result SQL, with immediate
exact repeats promoted to the prepared LRU. This preserves display/original
field and table metadata from `mysql_store_result()` while removing prepare and
statement-finalize cost for ordinary unique `mysqli_query()` result misses.
`MYLITE_MYSQLI_PREPARED_QUERY_RESULTS=1` keeps the always-prepared route
available for diagnostics. Focused API coverage verifies `mysqli_fetch_field()`
metadata, embedded-NUL result values, and text-query placeholder rejection.

The same non-isolated step now also enables
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`, which opens one harness-owned mysqli
connection after WordPress bootstrap and closes it at process shutdown. The
existing PHPUnit child-process lock-release patch also closes and reopens the
keepalive around child execution. This keeps the embedded runtime active across
short-lived WordPress mysqli objects while preserving ordinary `wpdb::close()`
and `mysqli_close()` behavior for application-visible handles; the database and
process-isolated shards keep the keepalive disabled.
The workflow now runs `tools/check-ci-production-builds`, also registered as
`tools.ci-production-builds` under production CTest, so CI fails if a CMake
timing path is moved back to developer presets, old developer build
directories, or unguarded WordPress timing settings. The audit also checks
that timing-bearing embedded, WordPress, and clang-tool step bodies retain
their production cache guards instead of merely carrying those guard strings
elsewhere in the workflow, and that embedded performance probes stay ahead of
embedded test steps.
Guarded ownerless SQL page-version reads are now enabled only when statement
refresh actually needs page-version WAL. In a continuous single-owner epoch,
local autocommit writes advance a separate local-native read boundary; eligible
same-runtime reads covered by that boundary avoid shared page-version pins and
the InnoDB file-read overlay. Autocommit writes outside that proof still seed
the real page-version read LSN for multi-process read-your-writes and retained
refresh. The active-reader pressure case covers the direct AUTO_INCREMENT
read-before-DDL shape that exposed the accidental overlay.
Eligible plain `SELECT`/`WITH` statements also enter the ownerless plain-read
scope when the current redo-visible state has no page-visible LSN and no
page-version read LSN yet: the handle publishes only the baseline read pin,
without enabling an external page boundary, so the read does not acquire
ownerless page-write ownership while an uncommitted peer transaction holds
page-write locks. The savepoint rollback SQL case covers this zero-boundary
peer-visibility shape before and after the writer commits.
The stats-enabled
embedded performance probe classifies page-version publish append attempts by
InnoDB page type and by native-support versus non-native-support class, while
preserving the legacy `snapshot_boundary` key for that non-native-support
complement and adding separate first-party counts for actual synthesized
snapshot-boundary appends. This is evidence for the remaining
page-publication write-volume work; it does not yet reduce page-version append
volume. The same stats-enabled probe now reports page-log payload bytes,
record-header bytes, and total record bytes, plus ownerless autocommit
per-insert byte averages, so CI production timings can distinguish append time
from full-page WAL write volume. The prepared DML reset path now avoids
`mysql_stmt_reset()` only after successful no-result statements with no result
metadata; the post-change reduced production attribution sample reported 500
ownerless autocommit resets with `0.064 ms` total reset time and
`0.000 ms` inside `mysql_stmt_reset()`, down from the previous `120.397 ms`
server-reset interval. The companion stats-off production sample reported
ownerless autocommit at `1520.62 ops/s` and ownerless transactional inserts at
`1567.77 ops/s`, while still showing that native page publication and InnoDB
commit/row mini-transaction costs remain the larger gap. This does not
complete native redo/checkpoint reconciliation. Ownerless no-result prepared
`INSERT`, `UPDATE`, `DELETE`, and `REPLACE` now defer native MariaDB
`MYSQL_STMT` creation until each protected `mylite_step()` execution. Public
`mylite_prepare()` still returns a MyLite parameter count for that subset by
tokenizing `?` markers, but native syntax/table/column validation can move to
first step in ownerless read/write mode. The tradeoff prevents a prepared DML
process from keeping unsafe native InnoDB table/dictionary state live while
peer ownerless writers wait at a readiness or statement boundary. The reduced
checksum-stress proof runs all-direct, one-prepared, and two-prepared writer
shapes with `MYLITE_OWNERLESS_CHECKSUM_STRESS_PREPARED_WRITERS`, while the
registered checksum stress preserves the default two prepared writers. The same
stats-enabled probe also classifies ownerless rollback-segment history flushes
by page type and by unique versus duplicate page identity, with accounting
guards that fail if the attribution no longer matches the existing flush total.
It also splits the exact native history flush into dirty-page needs checks,
known-page flush try time, exact-write AIO wait time, the AIO wait's
write-slot and doublewrite-buffer child waits, space-wide fallback time, and
final redo-log write time; the earlier reduced production attribution sample
reported `2.000` exact history flush pages per insert and `0.000` fallback
rounds, with the sampled cost dominated by exact-page try/wait work rather
than fallback. The first child-wait profile reported
`3.718 ms/insert` total exact AIO wait, `3.717 ms/insert` in
`write_slots->wait()`, and effectively zero doublewrite-buffer wait, pointing
the next optimization toward native data-file write drain and redo/checkpoint
proofs rather than doublewrite wait policy. A follow-up queue-depth profile
adds pending write-slot counts before and after that exact wait so production
logs show whether the wait is target-page write latency or global queue drain;
its first reduced sample reported `0.940` pending writes before the wait and
`0.000` after the wait per insert while exact flush pages remained `2.000` per
insert, so the current bottleneck does not look like unrelated global queue
drain. A bounded history WAL proof fast path now publishes the exact rollback-
segment and undo-header page images during the history mini-transaction and
skips the native exact history flush for pure autocommit `INSERT ... VALUES`
statements, including multi-row value lists, when both expected page images
were accepted by the ownerless page WAL, any transaction-deferred dirty page
images were accepted by the ownerless page WAL, and no publish failure
occurred. `INSERT ... ON DUPLICATE KEY UPDATE`, `INSERT ... RETURNING`, and
`INSERT ... SELECT` stay on the conservative unproven-statement bridge. The
reduced production attribution sample after the initial single-row proof change
reported `0.000` ownerless history flush pages and `0.000` exact history flush
pages per insert while keeping `3.570` native-support pages per insert and
`1.570` native-support elided pages per insert; the companion stats-off
production sample reported ownerless autocommit at `775.42 ops/s` versus
ordinary autocommit at `2272.02 ops/s` (`0.3413` ratio). Persistent undo-log
assignment and
history-list cache eligibility are also profiled so the production attribution
run can show whether an ownerless guard is blocking otherwise reusable one-page
undo logs. The ordinary-versus-ownerless attribution summary now also breaks
row insertion down into transaction-start, prebuilt, conversion, row-step,
post-processing, row graph, index-entry, clustered/secondary entry, clustered
and secondary low-level insert, and clustered pessimistic B-tree deltas, while
preserving the existing total row-insert and clustered optimistic B-tree keys.
A follow-up clustered-low attribution split separates index search,
duplicate-key checking, modify-record fallback, instant-root update, row-level
mini-transaction commit, and big-record follow-up from the existing clustered
optimistic and pessimistic B-tree timers.
The `e6efccff` production CI run reported a `0.249 ms/insert`
ownerless-minus-ordinary clustered-low delta, split between clustered
optimistic B-tree insertion at `0.116 ms/insert` and the row-level
mini-transaction commit at `0.129 ms/insert`; index search accounted for
`0.004 ms/insert`, and duplicate/fallback/pessimistic phases stayed at zero.
A follow-up optimistic B-tree attribution split now separates preflight,
lock/undo, tuple insertion, reorganization, adaptive-hash update, lock update,
and result counts within the clustered optimistic B-tree bucket. Row-level MTR
commit remains a separate nonzero performance target. The final reduced local
production sample with this split reported a `0.480 ms/insert`
ownerless-minus-ordinary clustered optimistic B-tree delta, with
`0.476 ms/insert` in `btr_cur_ins_lock_and_undo()`, `0.002 ms/insert` in
tuple insertion, `0.001 ms/insert` in preflight, zero
reorg/adaptive-hash/lock-update deltas, one successful optimistic insert per
row, and no fallback/error counts.
A follow-up lock/undo attribution split now separates setup, lock checking,
predicate/record lock checks, undo reporting, system-field writes, skip counts,
success counts, and error counts within `btr_cur_ins_lock_and_undo()`. The
first reduced local production sample with this split reported a
`0.158 ms/insert` ownerless-minus-ordinary optimistic lock/undo delta, with
`0.156 ms/insert` in `trx_undo_report_row_operation()`, `0.002 ms/insert` in
record lock checking, zero setup/system-field-write deltas, one primary-leaf
success per row, and no skip/error counts.
A follow-up undo-report attribution split now separates undo assignment,
cached-undo reuse, fresh undo creation, insert/update page reporting,
undo-report mini-transaction commit, success bookkeeping, page extension, and
error classes. The reduced 100-row local production sample reported a
`0.103 ms/insert` ownerless-minus-ordinary undo-report delta, with
`0.113 ms/insert` in the undo-report MTR commit bucket, near-zero page-record
encoding and bookkeeping deltas, zero assign/space/record-size/other errors,
ownerless cached-undo hits at `0.810` per insert, and fresh undo creates at
`0.190` per insert. The remaining insert-throughput target is therefore
ownerless mini-transaction page publication, not PHP startup or SQL row
encoding.
A follow-up slice now
allows MariaDB's existing cached-undo reuse only while the runtime remains in
the same continuous single-owner epoch already used for external-refresh skip
proofs; live peers, prior peers, active page-version pins, unmapped state, or a
missing redo/checkpoint baseline keep the conservative purge path. The current
reduced 100-row production attribution sample reported ownerless autocommit
cached-undo attempts at `1.000` per insert, hits at `0.810` per insert, fresh
creates at `0.190` per insert, zero ownerless cache-reuse skips, cached history
at `1.000` per insert, and an ownerless-blocked history-cache ratio of
`0.0000`; the companion stale-generation SQL selector proves the proof blocks
after another ownerless process has joined and left.
The WordPress CI
timing job now also enables a Release-build guard so `perf-probe` and
test-only PHPUnit phases reject missing, mismatched, or non-Release CMake
caches before reporting timings, and requires the transient WordPress MyLite
test database directory outside the repository worktree so timing runs do not
silently move onto the build-artifact filesystem. The harness prints the test
database parent filesystem type in timing logs after local production profiling
showed `Tests_Formatting_Emoji` process-isolated wall time changing from about
`89s` on the repo-backed test database to about `30s` on the default external
test database. CI also runs
`tools/require-cmake-release-build` against the normal, embedded, WordPress,
and clang-tools MyLite CMake caches, and
`tools/require-cmake-build-type MinSizeRel` against the embedded and WordPress
MariaDB embedded archive caches, so CMake-backed timing and test phases fail
early if they are not using production artifacts. Those guards are also repeated
inside the CI test/probe steps that report production timings, and the
WordPress timing harness prints the verified MyLite and MariaDB embedded CMake
cache build types before `perf-probe` or PHPUnit timing output, while
`tools/mariadb-embedded-build` rejects non-`MinSizeRel` caches before local
embedded `build`, `measure`, or warmed `ensure` output. The same probe now
also splits ownerless mini-transaction publish and commit-log phases so the
remaining autocommit gap can be attributed before a correctness-sensitive
publication optimization is attempted; the first reduced production sample
showed MTR commit-log work was significant but still much smaller than the full
ownerless `mysql_stmt_execute()` interval. Follow-up SQL/InnoDB handler
profiling shows the reduced ownerless autocommit sample spends most of the
measured `mysql_stmt_execute()` time in MariaDB/InnoDB commit plumbing and
row-insert internals, with `innobase_commit_low()` dominating the commit
boundary. Deep InnoDB profiling of the same reduced production probe shows the
dominant commit cost is in `trx_t::write_serialisation_history()` and its
commit mini-transaction, not in the post-commit ownerless visibility block:
400 ownerless autocommit inserts measured about `778 ms` in
`trx_commit_for_mysql()`, `750 ms` in write-history, `27 ms` in
`commit_in_memory()`, `20 ms` in the explicit ownerless visibility block, and
`230 ms` in `row_insert_for_mysql()` with about `176 ms` in clustered
optimistic B-tree insert. Ownerless page-version appends are now batched across
an InnoDB MTR publish scan with lazy append-lock acquisition and release before
active-reader boundary scans; the reduced stats-enabled production probe cut
append `fstat` time from about `21.8 ms` to `2.6 ms`, but the same 400-row
sample still spent about `120 ms` in page-log append and about `171 ms` in
commit-MTR page publication. Further optimization must therefore reduce
ownerless page-version write volume, native-support page publication, and
clustered row-insert costs before treating post-commit visibility release as
the bottleneck. Follow-up page-identity profiling showed the 400-row ownerless
autocommit sample is not dominated by repeated page identities: 3203
page-version publishes contained 3198 unique `(space_id,page_no,visible_lsn)`
fingerprints and only 5 duplicates, all native-support undo or transaction
system pages. A production prototype that skipped native-support page
publication under the current single-owner/no-pin proof cut that reduced sample
to 400 page publishes but failed throughput validation, including a stats-off
2000-row ownerless autocommit regression to about `168 ops/s` versus the prior
baseline around `295 ops/s`; native-support page publication therefore remains
enabled until broader redo/checkpoint reconciliation can prove both correctness
and throughput. The embedded and WordPress mysqli performance probes now also
emit compact `mylite_perf_summary_*` and `wordpress_perf_summary_*` lines for
CI branch/main timing comparison while preserving the detailed metric keys;
the embedded summary output includes warm open/close and active-runtime
reconnect subphase averages for open total, ownerless platform probe, runtime
start, runtime connect, system-table checks, dictionary handoff,
`mysql_server_init()`, close total, runtime release, ownerless reclaim, and
`mysql_server_end()` shutdown, so process-isolated PHPUnit startup cost can be
distinguished from in-process reconnect and engine throughput; ownerless
startup probing is split into a first-probe open/close sample and a cached
warm open/close sample so the one-time database-directory primitive proof does
not get averaged into recurring ownerless startup cost; the WordPress CI
performance probe uses five process/connect samples so PHP startup and
mysqli-connect averages are less load-sensitive while keeping SQL/write
iteration counts bounded, honors the separate connect-iteration control for
both process-plus-connect and in-process connect/reconnect samples, emits a
dedicated process-plus-connect iteration key next to that startup average, and
its summary keys include the requested Release build type plus the process,
connect, SQL, and write iteration counts;
stats-enabled ownerless autocommit probes add per-insert summaries for
MTR-published page-version volume, total MyLite page-publish hook calls,
page-log append calls and bytes, page-log encoding time, full/trailing-zero/
sparse-zero encoded record counts and bytes, compact-sparse subset record
counts and bytes, page-log payload counts and bytes for index, undo-log, SYS,
TRX_SYS, allocation/space-metadata, BLOB, and other page classes,
transaction-image, transaction-buffer, dirty-scan, and
buffer-pool-scan page-publish sources, native-support page ratio,
page-publish and page-log append time, page-write refresh/publish time,
commit-MTR publish time, InnoDB write-history time split by ownerless history-page lock, ownerless post-wait
refresh, rollback-segment latch, history-list mutation, write-history MTR
commit, ownerless rollback-segment-space dirty-page flush, page-type buckets
for that flush, the page-type-bucket sum and ratio guard, ownerless history
flush identity uniqueness, persistent undo assignment/cache-reuse decisions,
history cache eligibility and ownerless-blocked ratio, and ownerless release
time, ownerless visibility time, row-insert time, and clustered B-tree insert
time. The same stats-enabled attribution probe also emits ordinary insert
transaction and autocommit raw deep InnoDB counters, then summarizes ordinary
autocommit baselines and ownerless-minus-ordinary deltas for commit,
write-history, history-list, commit-in-memory, ownerless visibility,
row-insert, and clustered optimistic B-tree phases, so the remaining branch
gap can be separated from shared MariaDB/InnoDB insert cost. The total hook and
page-log append-call summaries are intentionally
separate from the MTR counters because commit-visible dirty-page publication
can reach the MyLite page-log path without incrementing the narrower MTR
publish counters; the visible-anchor profile also splits page-visible
publication into page-log sync lock/header/data-sync costs and durable
checkpoint update lock/read/write/data-sync costs, so CI can distinguish
filesystem sync pressure from local checkpoint bookkeeping before any
correctness-sensitive batching or deferral is attempted. Clean page-log syncs
may now be elided only when the current WAL file size and page-log header
generation match a process-local already-synced anchor; WAL-growth commits
still take the normal durability path. A bounded local
stats-enabled sample after adding that split reported page-visible hook cost at
`0.012 ms` per ownerless autocommit insert, while the broader checkpoint update
primitive ran four times per insert with `0.137 ms` total, mostly current-LSN
read time, and native clustered/undo mini-transaction deltas remained larger;
the write-history handoff
now waits natively only for the
rollback-segment tablespace through the history MTR LSN while leaving broader
global dirty-page waits in place for non-history commit fallback and
read-refresh paths. The first stats-off production sample after that change
reported ownerless autocommit at about `408 ops/s` versus ordinary autocommit
at about `1832 ops/s`, while the stats-enabled attribution sample still showed
the rollback-segment-space flush as a remaining `~1.0 ms/insert` cost; a
follow-up page-count attribution sample showed that wait flushing about `2.5`
rollback-segment-space pages per insert, and the follow-up page-type profile
splits that count into undo-log, index, FSP header, XDES, inode, allocated,
system, transaction-system, and other buckets; the stats-enabled production
attribution probe now fails if those buckets do not add up to the same
ownerless flush total. The post-boundary production sample before the history
WAL proof fast path reported stats-off ownerless warm open/close at
`359.230 ms` versus
ordinary `375.478 ms`, active-runtime reconnect overhead at `0.211 ms`,
ownerless direct/prepared read ratios of `0.9008`/`0.8629`, ownerless
transactional insert ratio of `0.7110`, and ownerless autocommit at
`601.94 ops/s` versus ordinary `2001.12 ops/s` (`0.3008` ratio). The companion
100-row stats-enabled attribution sample reported ownerless autocommit at
`407.20 ops/s` versus ordinary `2130.37 ops/s` (`0.1911` ratio), `4.570`
page-version records per insert, `3.570` native-support records per insert,
`0.433 ms/insert` in page-log append, and `0.561 ms/insert` in the native
rollback-segment-space dirty-page flush. The current history WAL proof
production attribution sample reports zero native history flush pages for the
same autocommit statement class. Current
Release branch/main WordPress profiling shows
focused database PHPUnit is not slower than main on the measured host, while
process-isolated PHPUnit remains dominated by child-process MyLite open and
close cost; the parent cleanup patch now filters typed static properties that
cannot hold objects before the cached static `wpdb` scan and reports
retained/skipped static-property counts plus per-child lock-release, runtime,
and reconnect averages on clean patched PHPUnit vendor trees. The WordPress CI
job now runs process-isolated PHPUnit with the static `wpdb` scan and
child-process profiling disabled while still closing the global WordPress
`wpdb` and eagerly reconnecting after each child; on the measured production
host, the `Tests_Formatting_Emoji` class changed from `41.816s` shell real with
full static scanning to `31.599s` with static scan disabled, and a same-session
A/B with static scan disabled measured `28.711s` shell real with child
profiling disabled versus `30.354s` with child profiling enabled. A
deferred-reconnect prototype failed later parent-side tests in that mixed class
and remains rejected. The harness now exposes
`MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD`, defaulting to eager
reconnect, so CI can skip parent reconnect only for process-isolated class
filters proven under production builds. A broad deferred trial over every
isolated class except `Tests_Formatting_Emoji` failed with 170 parent-side
`wpdb` connection errors in about `288.912s`, and focused trials kept
`Tests_Admin_WpAutomaticUpdater`, `Tests_Admin_WpUpgrader`,
`Tests_Filesystem_WpFilesystemDirect_Mkdir`, `Tests_Formatting_Emoji`, and
`Tests_Theme` on eager reconnect. The CI deferred shard is limited to
`Tests_Admin_ExportWp`, `Tests_Filesystem_WpFilesystemDirect_Chmod`,
`Tests_Functions_WpUniquePrefixedId`, `Tests_oEmbed_HTTP_Headers`, and
`Tests_Sitemaps_Sitemaps`; the final focused production trials for that set
passed as `20` tests in `88.635s` and `30` tests in `114.782s`, the combined
CI-shaped deferred shard passed `50` tests in `199.583s`, and the complementary
eager reconnect shard passed `271` tests in `199.256s`. An earlier mysqli
adapter slice kept a one-entry link-local prepared-statement cache for repeated
exact result-producing direct `mysqli_query()` SQL after rows and metadata had
been materialized into PHP result objects. The final CI-sized production WordPress
`perf-probe` after this change reported `SELECT 1` at `396.98 ops/s`, compared
with the earlier documented branch range around `260-290 ops/s`; process plus
connect/close remained `604.162 ms`, in-process connect/close `441.568 ms`,
and active-runtime reconnect `3.160 ms`, confirming that the slice improves
repeated result-query prepare/finalize overhead but does not reduce
process-isolated startup or ownerless autocommit publication cost.
The current production branch probe keeps that conclusion: after the
transaction-page publish dedup slice, the stats-off embedded sample reported
ordinary active-runtime reconnect at `1.222 ms`, ownerless active-runtime
reconnect at `0.971 ms`, ordinary prepared `SELECT 1` at `2466.47 ops/s`,
ownerless prepared `SELECT 1` at `2154.50 ops/s`, ordinary autocommit inserts
at `1190.87 ops/s`, and ownerless autocommit inserts at `767.28 ops/s`. The
matching WordPress sample used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56` and reported stock PHP startup at
`55.655 ms`, MyLite-extension PHP startup at `75.788 ms`, PHP process plus
MyLite connect/close at `603.801 ms`, in-process connect/close at
`393.118 ms`, active-runtime reconnect at `3.427 ms`, `SELECT 1` at
`380.95 ops/s`, point selects at `228.71 ops/s`, transactional inserts at
`385.62 ops/s`, prepared autocommit inserts at `315.41 ops/s`, and
direct-string autocommit inserts at `722.09 ops/s`. The production `^Tests_DB`
test-only phase reported PHPUnit `Time: 00:15.963` and
`wordpress_phpunit_seconds=27`. The slowdown visible in process-isolated
PHPUnit is therefore still startup/shutdown and embedded server lifecycle cost,
not an active reconnect or ordinary SQL regression.
The current production timing audit keeps CI on the same build shapes and now
also fails if the WordPress timing job collapses back into the harness `all`
phase or the old single-suite step. A fresh guarded production sample on
2026-06-10 reported ordinary embedded warm open/close at `361.594 ms`,
including `125.794 ms` in `mysql_server_init()` and `228.758 ms` in
`mysql_server_end()`, while ordinary active-runtime reconnect stayed at
`1.259 ms`. The matching CI-shaped WordPress `perf-probe` used the pinned
WordPress ref and guarded `Release`/`MinSizeRel` caches, reporting PHP process
plus MyLite connect/close at `552.251 ms`, in-process mysqli connect/close at
`382.206 ms`, active-runtime reconnect at `3.707 ms`, `SELECT 1` at
`410.02 ops/s`, point selects at `237.69 ops/s`, transactional inserts at
`340.01 ops/s`, prepared autocommit inserts at `332.09 ops/s`, and direct
autocommit inserts at `619.96 ops/s`. The remaining high-cost path is the full
MariaDB embedded lifecycle paid by process-isolated PHPUnit children and parent
reconnects for classes that cannot safely defer reconnect, not the steady
active-runtime SQL path.
The WordPress PHPUnit CI filters now use exact method-level process-isolated
shards instead of broad mixed-class filters, while the two class-level
`@runTestsInSeparateProcesses` files stay excluded from the non-isolated
bucket. CI-shaped production verification on 2026-06-10 passed the exact
deferred-reconnect shard as 31 tests in `198.815s` shell real, the exact
eager-reconnect shard as 22 tests in `121.294s` shell real, and the
non-isolated remaining shard as 28,687 tests in `2757.813s` shell real. This
does not reduce the ordinary WordPress suite volume, but it keeps
process-isolated timing from being inflated by unrelated non-isolated methods
in large mixed classes.
The long non-isolated shard now also excludes the whole `Tests_DB*` class
family with a leading `^(?!Tests_DB)` negative lookahead, matching the
dedicated `^Tests_DB` database shard and preventing database-prefix tests such
as `Tests_DB_Charset`, `Tests_DB_dbDelta`, and `Tests_DB_RealEscape` from
being timed twice.
The same production WordPress PHPUnit job keeps harness-owned JUnit timing
disabled by default after completed production branch runs without JUnit
reported the long non-isolated shard at `1156.633s` shell real in an earlier
run and `691.743s` shell real in the latest post-compressed-BLOB-size-matrix
run, while the latest split test-only PHPUnit steps summed to about
`14.1` minutes and the whole WordPress job completed in `21m52s`, below main's
`28:21.227` all-in PHPUnit body from the comparable one-shot harness run. The
harness-owned JUnit slowest-class and slowest-method report remains available
through
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` for targeted diagnostics, but it is not
enabled on the critical CI timing path. The CI timing path also sets
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1`, causing the harness to pass
`--no-logging` to PHPUnit when no explicit logging arguments are supplied, so
WordPress' default `phpunit.xml.dist` JUnit logger does not add XML generation
work to the split test-only timings. Diagnostic runs that need JUnit must set
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=0` together with
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`.
Current stats-enabled ownerless autocommit attribution also shows zero
non-SELECT page-version read probes after the InnoDB read-complete overlay was
limited to MyLite-classified plain reads. A 1000-row serial production
attribution sample after splitting total publish hooks from MTR-published page
versions reported `3.008` MTR-published page-version records per insert,
`5.470` page-publish hook calls per insert, `2.462` extra publish hook calls
per insert, and `5.470` page-log append calls per insert. Source attribution
then isolated the extra records to timer-driven buffer-pool scan publication:
the pre-fix 1000-row sample reported `0.955` buffer-pool scan publishes per
insert. The timer scheduler now requires one quiet scheduler interval after
the last ownerless statement before reclaiming; the matching 1000-row
production attribution sample reported `3.011` page-publish hook calls per
insert, `0.003` extra publish hook calls per insert, `3.010` page-log append
calls per insert, `0.000` buffer-pool scan publishes per insert, `3.008`
native-support records per insert, `1.004` native-support elided records per
insert, `2.004` published native-support records per insert, `0.090 ms/insert`
in page-log append, `0.113 ms/insert` in write-history, `0.125 ms/insert` in
row insert, and ownerless autocommit at `1176.94 ops/s` versus ordinary
autocommit at `1894.09 ops/s`. The matching stats-off 2000-row production
throughput sample reported ownerless autocommit at `890.53 ops/s` versus
ordinary autocommit at `2077.87 ops/s`, improving from the pre-fix `284.00`
ownerless autocommit ops/s sample that timer-driven buffer-pool scan
publication distorted. Newer page-log write-volume attribution reports the
matching payload and record-header bytes per insert for the same production
probe shape. The next performance target remains
page-version/native-support publication volume and native InnoDB
commit/row-insert cost, not timer-driven buffer-pool scan publication,
non-SELECT refresh probing, or post-commit release.
A follow-up production attribution slice now splits native-support page
publication into published versus elided page classes. The reduced
stats-enabled sample reported `2.000` published native-support pages per
insert, exactly `1.000` undo page and `1.000` transaction-system page, while
space-metadata native-support pages were elided in that sample. The remaining
hot-path page-publication target is therefore history-related native-support
proof, not broader space-metadata publication.
The completed production CI run for `64e48a6e` reported the default embedded
probe at `707.17` ownerless autocommit ops/s versus `2568.21` ordinary
autocommit ops/s, with ownerless warm open/close essentially equal to ordinary
warm open/close. Its stats-enabled attribution probe confirmed timer-driven
buffer-pool scan publication stayed at `0.000` publishes per insert and still
reported exactly `1.000` published transaction-system-bucket native-support
page plus `1.000` published undo page per ownerless autocommit insert. A
follow-up production attribution slice corrected that bucket interpretation:
the historical `trx_system` counter grouped `FIL_PAGE_TYPE_SYS` and
`FIL_PAGE_TYPE_TRX_SYS`. The 2026-06-10 reduced production sample reported
`1.000` published `FIL_PAGE_TYPE_SYS` page per ownerless autocommit insert,
`0.000` published `FIL_PAGE_TYPE_TRX_SYS` pages, `0.190` elided
`FIL_PAGE_TYPE_SYS` pages, `0.000` elided `FIL_PAGE_TYPE_TRX_SYS` pages, and
`0.000` canonical TRX_SYS byte-diff samples. The next bounded performance
target is therefore the generic InnoDB `FIL_PAGE_TYPE_SYS` page identity and
semantics, not blind TRX_SYS elision. The undo history-proof page remains out
of scope for blind elision. The matching default stats-off production probe
reported ownerless autocommit at `977.08 ops/s` versus ordinary autocommit at
`1537.18 ops/s`, keeping the current performance focus on remaining
native-support publication and native InnoDB commit/row-insert work. A
follow-up SYS identity attribution sample reported the measured SYS pages were
all undo-tablespace pages: `1.000` published SYS page and `0.200` elided SYS
pages per ownerless autocommit insert, first observed at
`(space_id=1,page_no=41)`, with zero system-tablespace fixed-page SYS
publication. The next native-support proof target is therefore undo-space SYS
page publication, not system-tablespace TRX_SYS or change-buffer/dictionary
fixed pages. A follow-up history-proof attribution slice then proved the simple
autocommit insert path's remaining published native-support pages are the
deliberate history-proof pages: the reduced 100-row production sample reported
`1.000` published history-proof rollback-segment page and `1.000` published
history-proof undo page per insert, matching the `1.000` published
`FIL_PAGE_TYPE_SYS` page and `1.000` published `FIL_PAGE_UNDO_LOG` page per
insert, with both pages also counted as blocked from blind native-support
elision by the active history-proof gate. The next optimization target is
therefore a cheaper or smaller history-proof mechanism, not blind elision of
these two page images under the current proof contract.
A follow-up page-log payload attribution slice keeps the same WAL format and
reports ownerless append payload by encoding mode and InnoDB page class. Its
reduced production sample reported all ownerless autocommit page-version WAL
records as sparse-zero encoded, with about `7969.470` payload bytes per insert:
`4225.950` SYS bytes, `3388.500` index bytes, `354.370` undo-log bytes,
`0.650` allocation/space-metadata bytes, and no TRX_SYS/BLOB/other payload.
The compact sparse page-log slice then added a 16-bit sparse metadata encoding
while preserving full-page checksum validation and the existing 32-bit sparse
fallback. In the reduced 100-row production attribution sample, all `3.020`
ownerless autocommit page-log records per insert used the compact sparse path
and page-log payload fell to `6701.670` bytes per insert: `4174.940` SYS
bytes, `2302.290` index bytes, `223.980` undo-log bytes, and `0.460`
allocation/space-metadata bytes. Longer 1000-row local production samples
reported ownerless autocommit at `1374.23` ops/s stats-off and `1434.30`
ops/s stats-enabled versus ordinary autocommit at `4031.68` and `4008.89`
ops/s; the stats-enabled 1000-row payload mix also included occasional full
index records and therefore reported `14903.235` payload bytes per insert
despite `2.875` compact sparse records per insert. Treat the compact encoding
as a WAL byte-reduction for zero-heavy page images, not as proof that the
history-proof page count or native commit cost is solved.
A follow-up compact-sparse composition attribution slice keeps the same WAL
format and splits those compact payload bytes into run metadata versus nonzero
page data. Its reduced 100-row production attribution sample reported
`6701.610` compact-sparse payload bytes per ownerless autocommit insert:
`1266.080` metadata bytes and `5435.530` data bytes. The SYS history-proof
bucket was strongly data-dominated at `51.200` metadata bytes and `4123.730`
data bytes per insert, while the index bucket was roughly split at `1086.220`
metadata bytes and `1216.060` data bytes per insert. The next write-throughput
target is therefore the rollback-segment SYS proof representation and
user/index page payload; the undo-header proof page and compact sparse run
metadata are not the primary byte-volume drivers in the simple insert sample.
A follow-up varint compact sparse page-log slice keeps the same page-version
record header and checksum contract but encodes compact sparse run metadata as
varuint16 gaps and run sizes when that is smaller than the 16-bit compact
header. Its reduced 100-row production attribution sample selected the varint
format for all `3.020` compact-sparse records per ownerless autocommit insert,
cutting page-log payload to `6078.160` bytes per insert and compact-sparse
metadata to `642.200` bytes per insert. Index payload fell to `1761.180` bytes
per insert with `545.110` metadata bytes and `1216.070` data bytes, while SYS
payload remained data-dominated at `4152.270` bytes per insert with only
`28.560` metadata bytes. This is a useful page-log byte reduction, but the
next larger write-throughput target remains the rollback-segment SYS proof data
and remaining user/index nonzero payload. A follow-up direct-varint encode
slice keeps those WAL bytes and flags unchanged but builds varint compact
sparse payloads directly from the page scan instead of first materializing and
reparsing the 16-bit compact payload. In the reduced stats-enabled production
probe, autocommit page-log append encode time moved from the prior `4.312 ms`
sample to `3.574 ms` for the same `302` varint records and `609070` payload
bytes; the bulk insert phase moved from `2.757 ms` to `2.293 ms` for the same
`152` varint records and `289499` payload bytes. Total append and throughput
samples remained noisy because payload-write timing varied, so this is
evidence for lower encode cost rather than completion of the write-throughput
work. A follow-up history-proof delta
attribution slice then measured accepted proof-page identity reuse without
changing WAL encoding. In the reduced 100-row production attribution sample,
rollback-segment proof pages and undo-header proof pages were each sampled
`1.000` time per ownerless autocommit insert, but the four-slot same-identity
diagnostic cache saw only `0.010` diff sample per insert for each role and
`0.950` evictions per insert. The only same-identity rseg diff changed
`0.110` bytes per insert, and the only same-identity undo diff changed `0.400`
bytes per insert, while the page-log still carried `6077.410` payload bytes per
insert, including `4152.330` SYS bytes and `1761.200` index bytes. That keeps
the next proof-representation work focused on history-proof identity churn and
broader redo/checkpoint proof design rather than a simple last-image delta
against the immediately previous proof page. A follow-up identity-attribution
slice then added low-memory per-role fingerprint tables. The reduced
stats-enabled production sample reported `1.000` rollback-segment proof
sample and `1.000` undo-header proof sample per insert, with `1.000` unique
identity, `0.000` duplicate identities, and `0.000` table overflows per insert
for each role. The bulk insert phase saw `24` unique identities and `1`
duplicate for each role across `25` proof samples. That proves the simple
autocommit path's history-proof payload is fresh identity churn, not repeated
same-page proof images hidden by the small byte-diff cache; a simple same-page
delta WAL format is therefore not the next runtime optimization for that path.
The fill-sparse page-log slice then targeted the measured SYS proof byte shape
without changing page-version count or proof semantics: it originally encoded
`FIL_PAGE_TYPE_SYS` records with a zero-filled page plus raw and repeated-fill
runs when that was smaller than the varint compact sparse payload. The reduced
stats-enabled production sample reported the same `3.020` page-log append
calls per ownerless autocommit insert, but total payload fell to `2010.720`
bytes per insert and SYS payload fell to `72.010` bytes per insert. The new
fill-sparse bucket accounted for `1.000` record and `72.010` payload bytes per
insert, split into `46.030` metadata bytes, `23.980` raw-data bytes, and
`2.000` fill bytes, while the remaining large buckets were `1779.030` index
payload bytes and `159.340` undo-log payload bytes per insert. Focused
primitive and SQL coverage now covers fill-sparse reconstruction, page-type
peeking, the history WAL proof, native-support page WAL elision, and multi-row
visible fast-path selectors. A follow-up index fill-sparse slice extends the
same byte-exact format to `FIL_PAGE_INDEX` pages while proving those records
remain snapshot-sensitive for oldest-reader checkpoint retention. The reduced
100-row stats-enabled production probe reported the same representative insert
payload mix, with `1779.010` index payload bytes, `159.340` undo-log bytes,
and `72.030` SYS bytes per insert; fill-sparse still selected `1.000` record
per insert for the SYS page, which shows the simple insert index image is not
dominated by repeated fill runs. The same sample reported page-log append at
`0.101 ms/insert`, append encode at `0.072 ms/insert`, ownerless autocommit at
`1472.89 ops/s` versus ordinary at `3137.60 ops/s`, and a stats-off sample
reported ownerless autocommit at `1241.04 ops/s` versus ordinary at
`2730.72 ops/s`. The index fill-sparse prefilter now inspects the already-built
compact payload before constructing a second index encoding and requires an
actual fill run before trying the full fill-sparse path. The reduced 100-row
stats-enabled production probe with that prefilter still reported only the
SYS fill-sparse record per insert and the same index-heavy payload mix, but
append encode returned to `0.059 ms/insert` and total page-log append to
`0.088 ms/insert`, with ownerless autocommit at `1364.26 ops/s` versus
ordinary at `2907.78 ops/s`. The next performance target is therefore native
commit/page-publication cost and a stronger user/index representation than
fill-run compression, not rollback-segment SYS proof byte volume.
Ownerless page-version reads now validate the WAL tail after a direct
page-index hit because the shared page index is an acceleration cache updated
after the append stream, not an authoritative visibility boundary by itself.
The generic InnoDB read-complete ownerless overlay is active only for
MyLite-classified plain `SELECT`/`WITH` page-version reads; non-SELECT DDL and
DML rely on explicit ownerless page-write refresh and publication paths. That
boundary prevents retained page-version images from being overlaid into native
DDL table rebuild reads, including compressed
`ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`/`8`/`16` rebuilds, while preserving the
reader-visible WAL overlay that protects monotonic page-version reads.
Ownerless live refresh and product no-live tablespace replay no longer treat
equal InnoDB page LSNs as proof that a retained page-version boundary is newer
than native state. When no-live reclaim discards retained page-version WAL, it
first publishes the current buffer-pool page set to the reclaim LSN, waits for
native dirty pages to flush, and then takes the native checkpoint so FK cascade
and DDL side-effect pages cannot be left only in volatile InnoDB state.
No-live non-DDL DML reclaim now also scans checkpointable user tablespace page
records and retains the WAL unless the native tablespace page on disk has a
`FIL_PAGE_LSN` newer than the record page LSN, or has the same `FIL_PAGE_LSN`
and byte-for-byte matches the retained payload. Live-peer reclaim keeps
checkpointable user data/index page records in the WAL until no-live reclaim
can make the native data file authoritative; support/allocation/system-only
records may still be compacted with live peers.
Live-peer reclaim also refuses to run from a writer runtime that has local
writes but has not consumed the current visible page-version WAL after those
writes, keeping immediate cleanup from racing a peer's native page refresh.
No-live writer reclaim still has to pass native page proof before truncating
retained page-version WAL.
The explicit-write slow commit path flushes through the current native InnoDB
log LSN before publishing a page-visible boundary, preventing a live reader
from pinning a raw latest boundary whose page image is neither in the ownerless
WAL nor durable on disk.
If a runtime only reads the current visible page-version WAL, final no-live
close leaves that WAL for recovery instead of truncating it without
writer-owned native page evidence; writer runtimes still use the normal
statement, timer, and close reclaim paths.
Ordinary exclusive read/write reopen with retained ownerless page-version WAL
or a nonzero ownerless checkpoint-visible boundary also refreshes clean
process-local InnoDB buffer-pool pages from that boundary before SQL can reuse
stale pages from an earlier embedded runtime in the same process.
Ownerless read/write runtime shutdown with live peers, or when no-live status
cannot be proven, now also refreshes to the latest ownerless external LSN and
waits for local dirty InnoDB pages through the max of that ownerless LSN and
the local InnoDB LSN before native teardown. This covers explicit transaction
stress where a process-local dirty page could otherwise survive until
`mysql_server_end()` and make native shutdown flush a stale clustered-page
image after peer commits, while proven no-live final close keeps the existing
native checkpoint/reclaim path.
Before a new explicit transaction writes a data/index page, MyLite
force-refreshes dirty process-local pages left by earlier work under the
ownerless page-write lock unless the same transaction already modified that
page, preventing stale full-page native flushes from erasing peer commits on
the same physical page.
The per-space transaction page-write gate stays statement-scoped so unrelated
explicit writers in the same tablespace are not serialized for the lifetime of
a transaction; real dirty page-write ownership remains transaction-scoped until
commit or rollback after page-level ownerless acquisition records the modified
page.
Ownerless `COMMIT` and full `ROLLBACK` ending an explicit transaction that has
performed local writes take the global ownerless write statement lock and
refresh current shared native state before executing the transaction-end SQL,
preventing one process-local InnoDB support-page image from hiding a peer's
concurrent commit evidence. Read-only transactions that only used locking
reads keep the conservative no-global-refresh state while active, but their
transaction end no longer waits behind a peer writer's global statement gate;
native InnoDB row/table locks remain responsible for the SQL wait.
Ownerless statement-lock acquisition defaults to the existing 60 second
internal wait, but a successful session `SET lock_wait_timeout = N` on that
handle now also bounds the ownerless statement-lock wait to `N` seconds so
tests and applications can fail fast on MyLite's directory-owned statement
gate without changing native InnoDB lock timeout semantics.
Explicit ownerless `READ COMMITTED` transactions remain non-pinning for plain
read statements: an eligible read can advance to the live page-version read
LSN when that transaction has not performed a local write or locking read and
no other live explicit ownerless transaction, shared read-write transaction, or
redo reservation is active.
Eligible autocommit page-version reads close the current InnoDB read view at
statement start and may advance to the latest page-version LSN when no other
native transaction or redo reservation is active and the older durable boundary
is retained by peer page-version pins, so a later statement can observe a peer
commit even while an older repeatable-read snapshot pin retains WAL for its own
reader. Ownerless page-write hooks also avoid taking page-write
ownership for SQL `SELECT`, including locking reads such as `SELECT ... FOR
UPDATE`; InnoDB row-lock and current-read paths remain responsible for
blocking, timeout, and deadlock behavior so timing evidence does not hide
SELECT waits behind page-write retry loops.
When the optional ownerless page-version WAL pressure limit rejects a write with
`MYLITE_BUSY`, the caller's transient autocommit read pin is released so the
blocked writer does not extend WAL retention after the external reader releases.
Direct autocommit result reads on pressure-limited handles also release their
transient pin after the statement result is produced.
The ownerless page-visible commit path uses initialized page-log append and
sync helpers for its already-open runtime WAL while the conservative public
page-log APIs still validate headers.
The WordPress mysqli adapter also skips redundant native parameter clearing for
fully-bound prepared statement execution, preserving partial-binding behavior
while reducing adapter work in prepared DML loops.

Ownerless page-version WAL records can now encode zero-heavy page images by
storing a compact 16-bit sparse nonzero-run list, the original 32-bit sparse
nonzero-run list, or a nonzero prefix plus a record flag, while retaining the
full page size and verifying checksums over the reconstructed full page image.
Primitive coverage verifies compact sparse zero-range, legacy sparse fallback,
tail-prefix, and zero-byte payload readback, append-session offset advancement
by encoded payload size, and checkpoint compaction of encoded retained records.
This reduces WAL byte volume for zero-heavy page images without skipping the
history-proof records or changing page-visible publication semantics.

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database directory | 🟡&nbsp;Partial | Implemented for read/write local directory paths with one active database directory per process, a `.mylite/` naming convention, validated format-1 metadata, an advisory directory lock, a native-storage baseline layout under the database directory, ownerless startup serialization, final no-live ownerless native DDL file-operation checkpoint evidence, and final no-live ownerless shutdown redo-prefix repair using MariaDB-valid checkpoint-page backups |
| Capability reporting | 🟡&nbsp;Partial | `mylite_capabilities()` reports build/profile-available concurrency modes; the embedded backend currently exposes same-process multi-handle support, ownerless shared read-only support, and ownerless read/write support, while each ownerless open still performs database-directory platform validation before runtime startup; `MYLITE_CAP_SHARED_READONLY` means MariaDB server `@@read_only=ON` plus user-visible read-only SQL through ownerless coordination, not InnoDB `innodb_read_only` startup |
| Read-only opens | 🟡&nbsp;Partial | `MYLITE_OPEN_READONLY \| MYLITE_OPEN_SHARED_READONLY` opens an existing directory through ownerless coordination, starts MariaDB with server `read_only=ON`, observes committed ownerless writer changes, rejects user-visible writes with `MYLITE_READONLY`, and rejects same-process ownerless read/write attachment while the read-only runtime is live; bare `MYLITE_OPEN_READONLY` remains reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds, while `mylite_exec_result()` exposes byte lengths plus display/original field and table metadata for one-shot result rows; `mylite_exec_result_with_metadata()` also emits field metadata for empty result sets so mysqli text-result queries can return zero-row result objects without using the prepared path; one-shot execution drains multi-result `CALL`/procedure output when MariaDB reports additional result sets and skips the redundant `mysql_next_result()` probe for ordinary single-result statements; native-storage coverage verifies MyISAM DDL/DML, row/index operations, and explicit InnoDB transaction/recovery behavior across reopen |
| Prepared statements | 🟡&nbsp;Partial | Reusable MariaDB prepared statements are exposed through `mylite_prepare()`, `mylite_step()`, `mylite_reset()`, and `mylite_finalize()` with 1-based parameter binding |
| Binary-safe values | 🟡&nbsp;Partial | Prepared text/blob bindings and column accessors use explicit byte counts; `mylite_exec_result()` and the default mysqli result-query path copy result values with explicit byte lengths; embedded NUL blob values are covered |
| Diagnostics | 🟡&nbsp;Partial | Open handles expose stable MyLite result codes, MariaDB errno, SQLSTATE, and message text; the default embedded profile keeps common MariaDB messages but may use compact generic text for uncommon inherited server errors |
| Ownerless pressure diagnostics | 🟡&nbsp;Partial | `mylite_ownerless_pressure_status()` reports the active page-version snapshot pin count, oldest pinned read LSN, raw ownerless page-version WAL bytes, configured soft limit, and whether the configured write throttle is currently reached for live ownerless handles; ordinary non-ownerless handles report zero ownerless pressure, thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling is covered when no peer process is live; live-idle coverage proves native-support checkpoint proof WAL is retained while a peer remains live and then checkpointed after that peer closes, while active-pin/live-writer coverage retains user page-version WAL; single-owner foreground statement reclaim uses a larger internal WAL budget when no native file-operation marker is pending while timer and close reclaim keep the normal threshold, and timer-driven checkpoint scheduling is covered when an idle open writer observes retained WAL after a shared read-only snapshot pin releases without another SQL statement or close |
| Warnings | 🟡&nbsp;Partial | MariaDB warning counts and indexed warning lookup expose level, code, and message text after statement execution |
| Affected rows and insert ids | 🟡&nbsp;Partial | Successful direct execution exposes affected rows for non-result statements and the last insert id |
| Raw `MYSQL *` as primary API | ➖&nbsp;Out&nbsp;of&nbsp;scope | Available only through a deliberate compatibility adapter |

## Engine Routing

| SQL engine request | MyLite status | Target behavior |
| --- | --- | --- |
| No explicit engine | 🟡&nbsp;Partial | MyLite follows MariaDB's compiled default storage engine; the current embedded profile resolves to InnoDB and covers no-engine table creation, metadata, persistence, and `@@default_storage_engine`; ownerless read/write opens reject attempts to switch session storage-engine defaults or overrides away from InnoDB |
| `ENGINE=MYLITE` | ➖&nbsp;Out&nbsp;of&nbsp;scope | No separate MyLite engine in the native-storage directory model |
| `ENGINE=InnoDB` | 🟡&nbsp;Partial | Explicit InnoDB tables use native InnoDB files inside the MyLite database directory; controlled transaction/recovery behavior and WordPress-shaped DDL are covered, while broader InnoDB features remain planned |
| `ENGINE=MyISAM` | 🟡&nbsp;Partial | Controlled create/alter/rename/drop/reopen and row/index coverage verifies `.frm`, `.MYD`, and `.MYI` files inside `datadir/`; ownerless read/write opens reject explicit MyISAM tables until per-engine coordination is designed |
| `ENGINE=Aria` | 🟡&nbsp;Partial | Explicit Aria table creation, row persistence, and `.MAI`/`.MAD` files under `datadir/` are covered; ownerless read/write opens reject explicit Aria tables until per-engine coordination is designed |
| `ENGINE=MEMORY` | 🟡&nbsp;Partial | Explicit MEMORY table creation is covered; table definitions survive reopen while rows remain process-local and empty after reopen; ownerless read/write opens reject explicit MEMORY tables until connection-local table behavior is designed |
| Dynamic external engines | ➖&nbsp;Out&nbsp;of&nbsp;scope | Default embedded profile does not load storage-engine plugins; explicit `ENGINE=BLACKHOLE` and `ENGINE=ARCHIVE` requests are covered as rejected, and ownerless read/write opens reject non-InnoDB engine requests before MariaDB execution |

## Directory Ownership

| Capability | MyLite status | Target behavior |
| --- | --- | --- |
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes and validates a MyLite-owned directory with `mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, process-local `run/`, and `concurrency/` metadata, lock, shared-memory, WAL, and checkpoint anchors; `.mylite/` is recommended but not enforced |
| Ownerless concurrency metadata | 🟡&nbsp;Partial | The directory owns a durable concurrency metadata file with format, MariaDB base, database UUID, concurrency generation, and exclusive-mode state, protected by a byte-range `PERSISTED_CONFIG` lock while it is created or validated; `MYLITE_OPEN_OWNERLESS_RW` uses this path instead of the process-wide `mylite.lock` |
| Ownerless platform proof | 🟡&nbsp;Partial | `MYLITE_OPEN_OWNERLESS_RW` and `MYLITE_OPEN_SHARED_READONLY` probe the prepared database directory for cross-process `MAP_SHARED` visibility, byte-range lock conflict, lock release on process exit, file grow/remap, and wait/wake support before ownerless runtime startup; a successful probe writes `concurrency/mylite-ownerless-platform.meta` with the database-directory device id so later opens skip the full probe until the proof is absent or the directory is on a different filesystem; hook-only coverage forces the probe to fail and verifies ownerless opens reject the directory while ordinary read/write opens remain on the non-ownerless path |
| Ownerless shared-memory file | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.shm` is created and grown under `RECOVERY` then `SHM_RESIZE` byte-range locks, validated through `MAP_SHARED`, starts with a fixed 128-byte MyLite header bound to the database UUID, and contains fixed process-registry, wait-channel, MDL lock-table, transaction-registry, read-view-registry, page-version pin registry, InnoDB lock-registry, redo-visibility, page-version-index, dictionary-generation, page-write lock-registry, and AUTO_INCREMENT high-watermark registry segments; clean opens preserve those segments, while dirty, rebuilding, invalid, no-live-process stale, or incompatible-format volatile state is rebuilt with an incremented recovery generation; volatile process active/live counts are read through `MAP_SHARED` mappings so recovery decisions see peer updates; ownerless SQL opens serialize core `mysql.*` compatibility-table bootstrap through `mylite-concurrency.lock` and the full embedded native startup, connection, dictionary-generation initialization, bounded retry after partial MariaDB embedded startup cleanup and redo-prefix restore, plus final no-live ownerless native DDL file-operation or `ALTER TABLE ... AUTO_INCREMENT` checkpoint evidence and shutdown redo-header repair through separate `mylite-runtime-startup.lock`, creating `concurrency/` before taking that startup lock, clearing process-global ownerless SQL/InnoDB hooks before MariaDB embedded shutdown while preserving native hook context until `mysql_server_end()` completes and keeping the shared-file deletion guard installed until shutdown finishes, and avoiding classic `fcntl()` same-file close release hazards; the 16,384-entry page-version index caches the newest WAL record offset per page, is replayed from `mylite-concurrency.wal` when `.shm` is rebuilt, distinguishes absent index entries from incomplete-index or older-snapshot lookups for diagnostics, keeps authoritative WAL scanning for page reads the index cannot prove, performs direct page-index reads and WAL scans under the existing page-log read guard instead of nested checkpoint read locks, classifies stats-enabled WAL-scan misses by page-key absence versus same-page-not-visible outcomes, uses process-local negative caching only after an authoritative WAL snapshot/scan has proven same-page absence instead of before the first index-miss WAL scan, and extends true no-same-page proofs across unrelated page-index generation changes with a WAL-generation/covered-offset tail cache; the index is reclaimed with live peers only when the page-version pin registry has zero active pins; active pins retain WAL until release; guarded ownerless SQL page-version reads cover direct or prepared `SELECT`/`WITH` statements at a live page-version read LSN while the page-visible LSN remains the durable recovery/checkpoint boundary, with a per-handle monotonic read watermark and a shared read pin opened before clean-page refresh; if no page-visible LSN exists yet, the baseline read pin still marks eligible plain reads as ownerless plain-read statements without exposing an external page boundary; successful direct `mylite_exec()` reads keep the handle pin after returning until a replacement read, non-read/current-read statement, error, close, or dead-owner cleanup releases it, and prepared result cursors keep the handle pin until the result is exhausted, reset, or finalized, so active result pins block single-owner statement/timer checkpoint scheduling while the same embedded runtime may still hold stale clean pages; when an eligible read retains a page-version read LSN, visible-boundary external refresh, clean-page refresh, and file-read overlays reject lower visible-boundary replacements for user data/index/blob pages while still refreshing native undo, system, allocation, and recovery pages; current live reads still accept current ownerless page images that advance the page; when a handle first observes a new ownerless process generation or advances its handle pin, clean-page refresh bypasses the single-owner skip because peer commits may already be native-checkpointed and reclaimed from WAL; repeatable-read and serializable transactions publish shared page-version pins for that read LSN on first consistent read, `START TRANSACTION WITH CONSISTENT SNAPSHOT` publishes the pin before SQL execution, and pins release on transaction end, rollback/close, or dead-owner cleanup, while transactions with local writes or locking reads avoid global refresh and clean-page refresh skips locally dirty buffer pages |
| Ownerless recovery anchors | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.wal` and `concurrency/mylite-concurrency.ckpt` are created under `RECOVERY` with fixed headers bound to the database UUID; guarded ownerless SQL writes page-version records after the `.wal` header, persists latest raw redo and page-visible LSNs in `.ckpt`, treats visible page-version WAL records as the no-live-process recovery authority, rewinds existing native InnoDB tablespace pages selected by commit LSN first and page LSN second, preserves matching native disk pages in product no-live replay when the native and retained WAL images have the same page LSN, retains complete committed page-version WAL records after no-live-process tablespace replay until native redo/checkpoint reconciliation can prove truncation safe, skips retained page-version records for tablespaces that no longer exist during product no-live replay, replays retained page-version WAL records into the shared page-version index when `.shm` is rebuilt, and seeds rebuilt or clean runtime-attached redo-visibility state monotonically from `.ckpt`; no-live `.shm` rebuilds checkpoint retained reader-boundary WAL without replaying stale page images when their remaining state is stale read-view/page-pin evidence without native writer recovery evidence, and focused SQL coverage verifies single-table dropped, same-schema and cross-schema same-statement multi-dropped file-per-table absence, ordinary-created file-per-table final state with secondary-index metadata/use, LIKE-copy, and CTAS-created file-per-table final states, same-name recreated and `CREATE OR REPLACE TABLE` replacement file-per-table final states, including `CREATE OR REPLACE TABLE ... LIKE` copied-shape replacement and `CREATE OR REPLACE TABLE ... AS SELECT` populated replacement, with page-0 space-id identity checks, cross-schema renamed file-per-table final state, truncated file-per-table post-truncate state, copy-style force-rebuilt file-per-table final state, multi-pair rename-swap final state, and multi-table dropped-schema absence through ownerless/native reopen before and after forced `.shm` rebuild; non-read-only close now advances local native LSN state when needed, and no-live close first publishes native `FILE_CHECKPOINT` evidence for completed DDL file-operation redo when it is the final live ownerless process, drains a real SQL `ALTER TABLE ... AUTO_INCREMENT` native file-op checkpoint marker after the final live peer closes, drains stale native file-op checkpoint markers even when no page-visible LSN exists, advances page-visible state to a newer raw latest LSN by publishing eligible native support/allocation/system buffer-pool pages and flushing native dirty pages, then refreshes external clean page state, forces a native InnoDB checkpoint, compacts page-version WAL records at or below the durable page-visible LSN covered by that native checkpoint, retains newer complete records, and replaces the shared page-version index before releasing checkpoint locks or leaves WAL scanning enabled as the safe fallback; this close-time reclamation path compacts user-page WAL with no live peers; with live peers, it compacts only when the proof scan has no process-local native page records after a nonblocking ownerless statement gate proves zero active page-version snapshot pins plus no active ownerless native write/recovery state; native-support proof and user data/index records remain retained until no-live close; active pins retain WAL until release, while page-version publish can still synthesize boundary records from a native page whose page LSN is at or below the oldest active pin so active readers and later post-release cleanup have a compact boundary image; bounded repeated same-row and distinct large-row expanding-page SQL writer pressure under a live reader are covered, an opt-in `mylite_open_config.ownerless_page_log_limit_bytes` soft cap returns `MYLITE_BUSY` for direct or prepared ownerless writes when active snapshot pins retain WAL at or above the configured byte limit, and `mylite_ownerless_pressure_status()` reports the active pin count, oldest pin LSN, raw WAL bytes, configured limit, and current throttle-reached state; thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling reuses the native reclaim path for user-page WAL when no peer process is live, and live-idle coverage retains native-support proof WAL while peers remain live and drains it after the final live peer closes, while user WAL remains covered by active-pin/live-writer retention tests, and the ownerless runtime scheduler can independently checkpoint retained WAL after a shared read-only snapshot pin releases while the writer remains open and idle; ordinary exclusive read/write opens enable page-version reads only when retained page-version WAL payload records exist, use a checkpoint-visible native clean-page refresh when `.ckpt` still proves a newer ownerless boundary after WAL compaction, and arm ownerless uncheckpointed file-operation recovery only when retained WAL, a native file-op checkpoint marker, or a valid saved redo-header backup proves prior ownerless redo/checkpoint suppression, so covered ownerless multi-writer commits and DDL policy handoffs remain visible through native exclusive reopen before and after forced `.shm` rebuild without taxing fresh ordinary opens; unsafe-hook SQL coverage now kills a writer after volatile `.shm` page-visible publication but before `.ckpt` persistence, kills after native checkpoint proof but before close-time page-log reclamation, and pauses after native checkpoint proof while a peer commits newer page-version records, proving normal reopen and forced `.shm` recreation do not depend on volatile `.shm` as the only copy of committed updates and that a resumed paused closer does not grow or invalidate already reclaimed WAL; page replay currently resolves existing tablespaces by InnoDB page-0 space id and still relies on the conservative native-file bridge for broader DDL/file lifecycle edge cases |
| Ownerless rename-create stale-reader replay | 🟡&nbsp;Partial | Focused SQL coverage updates an InnoDB file-per-table table while an older ownerless repeatable-read snapshot retains page-version WAL, renames that original table away, creates a different table at the original SQL name, writes both final tables, and verifies through ownerless/native reopen before and after forced `.shm` rebuild that the moved table keeps the original `SPACE` and the new original-name table has a distinct `SPACE`, with `.ibd` page-0 identity checks for both; broader DDL/file-lifecycle recovery remains partial |
| Ownerless CTAS post-create DML | 🟡&nbsp;Partial | Focused SQL coverage creates a CTAS file-per-table destination while another ownerless process pins a stale repeatable-read snapshot, verifies immediate numeric and payload `UPDATE`, `DELETE`, and `INSERT ... SELECT` statements against that CTAS table, retains page-version WAL while the reader pin is live, and verifies ownerless/native reopen before and after forced `.shm` rebuild; active-reader pressure coverage also blocks post-create `UPDATE`, `DELETE`, and `INSERT ... SELECT` against an existing CTAS destination while retained WAL is at the configured soft limit, verifies no side effects, then verifies CTAS DML succeeds after the pin releases; `tools/ownerless-ctas-dml-trace` emits deterministic CTAS create/update/delete/insert SQL plus repeatable snapshot reader and final oracles for external harness input, with focused Docker-backed MariaDB 11.8 scale-2 replay evidence; exhaustive CTAS DML/crash matrices and randomized external stress remain planned |
| Ownerless consistent-snapshot baseline | 🟡&nbsp;Partial | `START TRANSACTION WITH CONSISTENT SNAPSHOT` on an ownerless read/write handle publishes its page-version pin before SQL execution; when `.ckpt` has no page-visible LSN and `mylite-concurrency.wal` has no payload records, the pin path can seed `.ckpt` and shared redo-visible state from the current native InnoDB checkpoint LSN, giving active-reader boundary synthesis a nonzero baseline without installing ownerless hooks for fresh ordinary opens or doing broad startup-time checkpoint seeding |
| Ownerless page-version log primitive | 🟡&nbsp;Partial | A first-party fixed-record page-version log primitive can initialize a log, initialize at a payload offset, serialize appends with a byte-range lock, store real InnoDB `space_id`/`page_no` pairs including zero, snapshot a stable log end, read a record by WAL offset, reject direct-offset reads whose record identity does not match the expected page, replay committed record metadata for shared-index rebuild, read the latest version visible at a commit LSN without holding the append lock while scanning, compact records at or below a safe commit LSN while retaining newer records, preserve active snapshot data-page boundaries in primitive checkpoint evidence, retain newer snapshot-page records for the general multi-pin path, compact checkpointed post-snapshot records in the single-pin primitive path, require boundaries for R-tree/index/blob/instant/unknown/unrecognized page types while allowing only explicit undo/allocation/tablespace-header/extent/transaction-system/change-buffer/system support pages to bypass the oldest-snapshot check and be dropped after native checkpoint proof, run a checkpoint-completion callback before releasing checkpoint locks, safely truncate the log when every complete record is at or below a safe commit LSN, report missing pages and undersized buffers, durably sync the log before page-visible publication unless the current WAL size and header generation exactly match a process-local already-synced anchor, write embedded-build payload checksums with a MariaDB CRC32C-derived 64-bit value while accepting retained legacy FNV64 checksum records, and tolerate incomplete or checksum-corrupt tail records; primitive tests cover same-process and cross-process append/read behavior, snapshot-bounded reads, corrupt-tail recovery, checkpoint compaction with retained offsets, checkpoint-time page-index replacement, same-version stale page-index publish rejection, checkpoint write-lock blocking behind a cross-process long reader, stale-index WAL-scan recovery, stale-index identity rejection, safe all-record checkpoints, page-index clearing, page-index replacement after WAL-scan fallback, replaying record offsets into a page index, initialized clean sync elision by exact size/generation anchor with append-forced resync, active-pin multi-pin retention versus single-snapshot compaction, native support-page checkpoint drop, R-tree missing-boundary rejection, and applying visible page records to existing native tablespace files, including reading native pages at or before a target snapshot LSN, rewinding a higher-LSN disk page, rewriting a same-LSN different-image disk page, preserving a native same-LSN page in product replay mode, selecting same-page replay winners by the page-log latest-visible commit ordering, strict missing-tablespace rejection, and product-mode dropped-tablespace skipping; guarded SQL tests verify dirty page images are appended to `mylite-concurrency.wal`, indexed in `mylite-concurrency.shm`, replayed into a rebuilt `.shm` page index, reclaimed on no-peer close after native checkpoint evidence, native-support proof WAL is retained while an idle live peer remains open with no page-version pins and reclaimed after that peer closes, user page-version records are retained by live writer, live snapshot pin, and pressure coverage, live repeatable-read snapshot pins block prefix compaction until boundary proof exists, synthesize native page boundaries while a live repeatable-read snapshot pin is active, retain multi-pin newer-record coverage through the active-pin hook path, use hook-backed single-active-pin primitive reclaim to drop checkpointed post-snapshot records when boundary proof is complete while product close-time reclaim retains WAL until active pins release, retained after a killed pinned reader is cleaned while another ownerless peer remains live, then reclaimed after that peer exits, partially compacted when a peer commits newer records while an older closer is paused at native checkpoint proof, and recover after a process is killed at the deterministic pre-truncate recovery-checkpoint fault, after volatile page-visible publication but before `.ckpt` persistence, after native checkpoint proof but before close-time page-log reclamation, or killed with uncommitted same-page updates; ownerless SQL and ordinary exclusive read/write reopens can read page versions for direct and prepared `SELECT`/`WITH` statements at a live page-version read LSN, including transactions with local writes whose own uncommitted redo can hold back the durable page-visible LSN; locally dirty buffer pages remain resident during clean-page refresh; non-forced page-version write refresh accepts strictly newer page-version images but does not overwrite a same-LSN or newer clean local page with an older page-version image; after startup, InnoDB read completion validates ownerless page identity/checksum in a temporary buffer, including `space_id=0` system-tablespace pages, and overlays the disk frame only when the disk frame is invalid for the expected page or older by page LSN; broader DML/DDL and reconstruction of missing DDL-created tablespaces remain on the conservative native-file bridge; the page-visible LSN advances after transaction-owned dirty page images for the commit have been published and the page-version WAL is durably synced or proven unchanged from that process-local synced anchor, with MTR-proven autocommit writes and transaction-deferred dirty pages proven by transaction-page publication allowed to skip the native dirty-page flush while DDL, unproved transaction-deferred dirty pages, rollback/deadlock cleanup, and any MTR publish skip/failure keep the conservative flush bridge; page-visible publication is blocked while another live ownerless process is inside an explicit transaction or the shared transaction registry has active read-write transactions, rollback no longer publishes a post-cleanup global `log_get_lsn()` boundary, and global dirty-page scans publish only native support/allocation/system pages instead of user data/index pages; hook tests verify the page-version visibility LSN is scoped to the executing SQL thread |
| Ownerless coordination primitives | 🟡&nbsp;Partial | POSIX file-backed `MAP_SHARED` visibility, grow/remap behavior, byte-range lock conflicts, lock release on process exit, internal mapped latch wait/wake plus timeout behavior, fixed-width owner-generation-aware shared latch words, internal cross-process process-slot allocation, heartbeat update, live-slot counting, stale-slot cleanup including exited-process cleanup, dead-owner lock cleanup, an internal cross-process metadata lock-table primitive with repeated-owner reference counts, same-owner mode upgrades, MariaDB-style granted compatibility for schema IX/S/X and table S/SH/SR/SW/SU/SRO/SNW/SNRW/X modes, stable ownerless MDL schema/table key hashing, an internal cross-process transaction registry primitive for monotonic transaction IDs, active-ID snapshots, oldest-active tracking, stale end rejection, and owner-scoped active-count checks, an internal read-view registry for purge-visible read-view publication and cleanup, an internal page-version pin registry for cross-process snapshot read-LSN publication, oldest-LSN snapshotting, owner cleanup, and slot exhaustion, an internal InnoDB table/record lock-registry primitive with MariaDB-compatible table/gap/insert-intention conflict coverage, conservative same-page physical-X resource conflict coverage for separate process-local buffer pools, shared wait-edge publication, wait cleanup, cross-process wait-cycle detection, final timeout availability rechecks, wait-only missed-wakeup coverage, and table-lock waiter-death owner-cleanup coverage to avoid stale wait entries after missed deadline wakes or killed waiters, a separate internal page-write lock registry for X/SX page-latch write ownership that must not be starved by row-lock-heavy transactions, an internal AUTO_INCREMENT registry that preserves per-table next-value high watermarks across ownerless peers, and an internal redo-state primitive for owner-generation-aware redo latch ownership, nested local entry, latch-free latest-LSN observation, raw latest LSN publication, page-visible LSN publication, monotonic checkpoint seeding, serialized append-range reservation, coalesced out-of-order completed redo ranges, contiguous written-LSN tracking, and dead-owner cleanup are covered as platform evidence; unsafe-hook SQL coverage now kills a writer after shared transaction registration but before the update proceeds, proves a later writer cannot commit past an earlier unwritten redo reservation, and kills a writer after a completed redo write but before latest-LSN checkpoint publication, proving live-peer cleanup remains busy while no-live reopen rebuilds without applying interrupted updates; product opens now allocate and release a directory process slot, validate the transaction, read-view, page-version pin, InnoDB lock, page-write lock, AUTO_INCREMENT, and redo-visibility segments, preserve dead-owner recovery-sensitive state while live peers remain, return busy instead of deleting that state, and rebuild stale volatile coordination after no live owners remain |
| Ownerless MDL hook surface | 🟡&nbsp;Partial | MariaDB's embedded MDL ticket lifecycle has a MyLite hook point for schema/table metadata-lock acquire and release, including cloned tickets, upgrades, downgrades, and release balancing; `libmylite` registers it against the directory-backed MDL lock-table segment using the runtime process-slot owner for ownerless opens, maps schema/table tickets to mode-aware granted-lock compatibility, and covers cross-process `ALTER TABLE` timeout behavior behind an active transaction |
| Ownerless InnoDB transaction and read-view hook surface | 🟡&nbsp;Partial | InnoDB maximum transaction ID reads, transaction ID allocation, read-write transaction registration, transaction serialisation-number assignment, active transaction snapshots, deregistration, read-view publication/removal, and purge oldest-view snapshotting have guarded MyLite hook surfaces covered by embedded InnoDB SQL tests; active transaction snapshot reads retry transient ownerless hook errors before retaining the persistent-error abort path, with embedded hook coverage for an injected transient snapshot error; internal or recovered transactions that were never registered in the ownerless shared registry still receive serialisation numbers from the shared monotonic sequence, and missing deregistration is treated as a no-op; ownerless opens install those hooks against directory-backed shared state |
| Ownerless InnoDB lock hook surface | 🟡&nbsp;Partial | InnoDB table-lock creation/removal, record-lock bitmap bit set/reset, waiting-lock grant, record-lock object dequeue, wait enqueue/reset, discard paths, AUTO_INCREMENT counter reservation, and X/SX data-page latch write ownership have guarded MyLite hook coverage that mirrors granted native locks and local wait edges into the directory-backed InnoDB lock-registry segment, while B-tree/external-value page writes use a dedicated page-write lock-registry segment; locks acquired before `trx_t::id` exists use a stable transient MyLite lock identity until release, and explicit-transaction page-write locks acquired under a transient page-write identity remain transaction-scoped until commit or rollback; embedded SQL tests verify lock entries appear during real InnoDB write transactions and DDL locking-read paths, same-owner conflicts remain native InnoDB's responsibility while local row-lock waits publish and clear shared wait entries, ordinary `REC_NOT_GAP` row locks keep record-level identity, physical same-page X resources serialize only when the native lock has no record/gap flags, insert-intention checks honor peer gap/next-key locks, ownerless simple inserts reserve AUTO_INCREMENT values through a shared table-ID-keyed high-watermark registry, and row-lock-heavy transactions do not starve page-write serialization; granted locks release on commit after dirty pages are flushed through the transaction commit LSN, rollback, and normal close; dead-owner cleanup no longer removes granted lock or redo-visibility state while live peers remain because that state is transaction-recovery evidence. Pre-grant reservation prevents a local grant when the shared registry already contains a conflicting external record lock, synthetic same-page cross-process X resources serialize to avoid process-local buffer-pool page-image overwrites, queued same-page waiters cannot be bypassed by new arrivals, dirty page-write ownership is acquired only when a persistent page becomes dirty, deferred dirty page-write deadlocks and rollback-segment history commit page-write deadlock reports retry instead of proceeding unlocked or asserting, mini-transaction-local page-write acquisitions are released even when no modify memo remains, explicit transaction handler write locks use statement-scoped tablespace gates as statements open tables and release those gate markers at statement end while preserving real dirty page-write locks until commit or rollback, a cross-process external record conflict waits and wakes after release, undo segment creation holds ownerless tablespace-allocation serialization through its mini-transaction, undo/system page versions publish at mini-transaction scope after ownerless redo is written while user data/index pages remain transaction-visible, later undo/system writes in explicit transactions still acquire page-kind-aware ownerless pre-write ownership after user data pages have been deferred, autocommit ownerless statements refresh local InnoDB redo/page state and durable tablespace header/allocation metadata between statements, with a single-owner epoch proof skipping non-forced page-write, space-metadata, and explicit-transaction buffer-pool first-write refresh only after a checkpoint baseline exists and while no peer process, peer-owned page-version pins, or peer-history invalidation are present, while explicit transactions avoid global refresh and peer/live cases keep conservative buffer-pool refresh, ownerless read paths advance the local durable LSN for externally flushed pages, rollback-segment history commits refresh the relevant tablespace header and current first history-list undo page before validating free-list bounds and splicing file-list links, ownerless embedded waits honor the current SQL session lock-wait timeout when the InnoDB transaction lacks `trx->mysql_thd`, cross-process deadlocks return MariaDB errno 1213, shared-registry timeout maps to MariaDB errno 1205 after a final availability recheck, and post-wait refresh targets the waited record page instead of globally evicting pages from an active writer transaction |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and MyISAM table metadata lifecycle is covered for `db.opt`, `.frm`, create, alter, rename, and drop paths inside `datadir/` |
| InnoDB files | 🟡&nbsp;Partial | Representative InnoDB tablespace, redo, undo, and temporary files are configured and covered inside the MyLite database directory; ownerless read/write opens use a private InnoDB temporary tablespace under each process runtime `tmp/` directory so same-named temporary tables remain connection-local across peers; ownerless read/write mode rejects explicit `ALTER TABLE ... DISCARD/IMPORT TABLESPACE` plus create/alter `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options, including representative idempotent, replacement, and temporary create-table spellings, before unproven native file-layout or detach/import paths; buffer-pool dump/load is disabled at startup and matching dynamic InnoDB buffer-pool dump/load variable assignments are rejected because the advisory `ib_buffer_pool` file is unsafe under concurrent embedded processes and is not needed for durability |
| MyISAM files | 🟡&nbsp;Partial | Controlled lifecycle and native table operation coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across create, row DML, copy alter, rename, drop, and reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage verifies `.MAI` and `.MAD` files under `datadir/` |
| MEMORY definitions | 🟡&nbsp;Partial | Explicit MEMORY table coverage verifies persistent table metadata under `datadir/` and empty row state after reopen |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use per-runtime `tmp/<runtime-id>/`, `run/<runtime-id>/`, and `mylite.lock` inside the database directory; clean close removes the current runtime's children and prunes an empty `run/` root, clean exclusive open replaces stale inactive runtime children after taking the directory lock, and `:memory:` uses a transient runtime directory that is removed on final close |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-surface policy coverage rejects or disables known server-owned paths that could create replication, binlog, performance-schema, or `mysql.*` sidecars outside the supported application-storage model, and ownerless active-reader pressure coverage verifies representative process-control, account/grant, plugin, binlog, logging, query-cache, event/scheduler, and host-file import statements keep the explicit server-surface policy error instead of becoming retryable pressure-limit failures; table DDL rejects `DATA DIRECTORY` and `INDEX DIRECTORY` options before native engines can route table files to caller-named locations outside the MyLite directory, including ownerless coverage for representative create, alter, and partition-level directory-option spellings |

Closed-directory copy coverage now binds `mylite-concurrency.shm` headers to
the actual shared-memory file identity and verifies a copied closed database
rebuilds volatile `.shm` state before attaching the runtime. Copying an open
directory remains unsupported until a coordinated backup protocol exists.

Ownerless recovery-anchor evidence includes hook-only SQL coverage that
corrupts saved redo-header backup magic, format, header size, payload size,
recorded redo size, saved prefix, and truncation boundaries, proving malformed
`concurrency/mylite-redo-header.bin` files do not arm the ordinary-open
recovery bridge.

Ordinary exclusive read/write close now captures a validated InnoDB redo
startup prefix before final embedded shutdown and conditionally restores it
after `mysql_server_end()` only when the post-shutdown prefix no longer passes
MariaDB-current checkpoint validation. Focused production coverage creates an
InnoDB table and verifies at least five full ordinary close/reopen cycles with
row updates, and the CI-shaped production performance probe keeps full
startup/shutdown timings separate from active-runtime reconnect timings.

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM create, drop, and rename lifecycle is covered for native metadata and engine files; explicit InnoDB, Aria, MEMORY, MariaDB default-engine create, `CREATE TABLE ... LIKE`, and `CREATE TABLE ... SELECT` coverage is also present, including ownerless peer-refresh coverage for idempotent `CREATE TABLE IF NOT EXISTS` / `DROP TABLE IF EXISTS`, `CREATE OR REPLACE TABLE` replacement of an existing InnoDB table definition and rows, `LIKE`, CTAS, ordinary inline secondary `INDEX` creation with duplicate inline key-name failure and no leaked table, same-schema parent/child foreign-key `RENAME TABLE` metadata/enforcement, cross-schema parent/child foreign-key `RENAME TABLE` metadata/enforcement, same-schema and cross-schema multi-pair parent/child foreign-key `RENAME TABLE` metadata/enforcement, cross-schema InnoDB `RENAME TABLE` with `.frm`/`.ibd` movement between schema directories, and a multi-pair InnoDB rename cycle that swaps tablespace identities; `CREATE TABLE ... DATA DIRECTORY` and `CREATE TABLE ... INDEX DIRECTORY` are rejected to preserve single-directory storage, ownerless policy coverage verifies create-time table-directory option rejection before external paths are created, and ownerless read/write mode rejects partitioned `CREATE TABLE` plus create-time `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options, including idempotent, replacement, and temporary create-table spellings, before entering unproven partition metadata, storage-option, and file-lifecycle paths |
| `ALTER TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `ADD COLUMN` and copy-style `ADD KEY` lifecycle is covered across close and reopen; representative default-engine InnoDB column modify/change and index add/drop changes are covered, ownerless peer-refresh coverage exercises an online/in-place index alter, explicit online DDL option variants (`ALGORITHM=INSTANT, LOCK=DEFAULT` column add/drop plus stored-column placement and column rename, `ALGORITHM=INSTANT` placed stored-column ADD with `LOCK=SHARED` and `LOCK=EXCLUSIVE`, instant virtual generated-column ADD with `LOCK=SHARED` and DROP with `LOCK=EXCLUSIVE`, `ALGORITHM=NOCOPY, LOCK=NONE` secondary-index create and drop, `ALGORITHM=NOCOPY, LOCK=DEFAULT` secondary-index create and drop, `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE` secondary-index create and drop, `ALGORITHM=INPLACE, LOCK=NONE` secondary-index create, `ALGORITHM=INPLACE, LOCK=SHARED` secondary-index create, unique secondary-index create and drop with `ALGORITHM=INPLACE, LOCK=SHARED`, `ALGORITHM=INPLACE, LOCK=DEFAULT` secondary-index create and drop, `ALGORITHM=COPY, LOCK=EXCLUSIVE` column/rebuild paths, and explicit no-lock index ignored/not-ignored toggles), secondary-index rename, index ignored/not-ignored toggles, primary-key replacement with hook-build crash recovery for plain and composite direction replacements plus idempotent ADD PRIMARY KEY no-op preservation and crash recovery, descending-key metadata, composite direction metadata, AUTO_INCREMENT-column preservation, and AUTO_INCREMENT descending-key metadata, foreign-key add/drop, generated-column add/drop and same-kind expression replacement, table charset conversion, row-format rebuild including focused compressed `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`, `KEY_BLOCK_SIZE=4`, and `KEY_BLOCK_SIZE=16` rebuilds with peer metadata refresh and native ZBLOB page evidence, table comment metadata, `ALTER TABLE ... FORCE` rebuild, column default set/drop, column add/modify/rename/drop ALTERs, idempotent `ADD COLUMN IF NOT EXISTS` / `DROP COLUMN IF EXISTS` behavior with duplicate-add errno 1060 plus hook-build crash recovery for duplicate-add and missing-drop no-op branches, hook-build crash recovery for missing `MODIFY COLUMN IF EXISTS`, `RENAME COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`, `ALTER COLUMN IF EXISTS SET DEFAULT`, and `ALTER COLUMN IF EXISTS DROP DEFAULT` no-op branches including missing rename/change/default preservation on generated-column/CHECK expression tables, explicit InnoDB instant ADD/DROP/reorder column metadata from another process, and explicit instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants from another process, and concurrent ownerless DDL allocation coverage exercises online index add/drop replacement; `ALTER TABLE ... DATA DIRECTORY` and `ALTER TABLE ... INDEX DIRECTORY` are rejected to preserve single-directory storage with ownerless policy coverage for representative alter-time directory-option spellings, while ownerless read/write mode rejects partitioning plus add/drop/rebuild/optimize/analyze/check/repair/coalesce/truncate/reorganize/exchange/convert/remove partition-maintenance ALTERs before entering unproven partition metadata and file-lifecycle paths, rejects `DISCARD/IMPORT TABLESPACE` before unproven native file detach/import paths, and rejects alter-time `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options before unproven storage-option file-layout paths; broader edge cases remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | 🟡&nbsp;Partial | Representative default-engine InnoDB standalone index create/drop is covered through MariaDB DDL and native engine metadata; ownerless coverage verifies already-open peer refresh for standalone InnoDB secondary-index create/use/drop, idempotent secondary-index create/drop with duplicate-create errno 1061, hook-build crash recovery for duplicate top-level `CREATE INDEX IF NOT EXISTS` preserving the original key part, missing top-level `DROP INDEX IF EXISTS` preserving the real index, duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` preserving the original key part, and missing `ALTER TABLE ... DROP INDEX IF EXISTS` preserving the real index, `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and `ALTER TABLE ... DROP INDEX IF EXISTS` table-element idempotency, `CREATE OR REPLACE INDEX` replacement of an existing index name over a different key part, multi-column unique-index create/enforce, idempotent `CREATE UNIQUE INDEX IF NOT EXISTS` no-op preservation with duplicate plain-create errno 1061 plus hook-build crash recovery for duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` preserving the original unique key, `CREATE OR REPLACE UNIQUE INDEX` enforcement movement to a replacement key definition, `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` create/no-op preservation with duplicate plain-add errno 1061 plus hook-build crash recovery for duplicate ALTER unique add preserving the original unique key, missing/repeated `ALTER TABLE ... DROP INDEX IF EXISTS`, and drop semantics, unique descending secondary-index metadata/enforcement/drop semantics, unique prefix secondary-index metadata/enforcement/drop semantics, unique prefix-plus-direction secondary-index metadata/enforcement/drop semantics, utf8mb4 prefix secondary-index character-count metadata/enforcement/drop semantics, unique TEXT/BLOB prefix secondary-index metadata/enforcement/drop semantics, unique TEXT/BLOB prefix-plus-direction secondary-index metadata/enforcement/drop semantics, descending secondary-index `COLLATION = 'D'` metadata refresh/use/drop, mixed ASC/DESC composite-index `COLLATION` metadata refresh/use/drop, prefix-plus-direction secondary-index `SUB_PART`/`COLLATION` metadata refresh/use/drop, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix secondary-index `SUB_PART` metadata refresh/use/drop, TEXT/BLOB prefix-plus-direction secondary-index `SUB_PART`/`COLLATION` metadata refresh/use/drop, and final index absence through ownerless/native reopen before and after forced `.shm` rebuild; ownerless read/write mode rejects top-level, ALTER, inline, and idempotent `FULLTEXT` and `SPATIAL` index DDL before entering unproven special-index native storage paths |
| Generated-column secondary-index DDL | 🟡&nbsp;Partial | Ownerless coverage verifies standalone `CREATE INDEX`/`DROP INDEX` over deterministic stored and virtual generated InnoDB columns, unique generated-column indexes, prefix generated-column indexes with `SUB_PART` metadata, mixed-direction composite generated-column indexes, accepted explicit `NOCOPY`/`INPLACE` generated-column index add/drop options, already-open peer `INFORMATION_SCHEMA.STATISTICS` refresh, forced-index reads while present, generated-value recalculation after peer DML changes base columns, indexed stored and virtual generated-column expression replacement with forced-index reads over recalculated replacement values, forced-index failure after drop, and ownerless/native reopen before and after forced `.shm` rebuild; upstream blocked-function cases for MyLite-trimmed `GET_LOCK()`, `SLEEP()`, and `UUID_SHORT()` remain under the server-utility SQL function policy because they are rejected before generated-column validation; ownerless policy coverage verifies MariaDB errno 1903 and side-effect-free failure for create-time, replacement, and existing-column generated-column primary-key DDL, errno 1901 and side-effect-free failure for representative nondeterministic stored generated expressions, aggregate/subquery/time/session/nondeterministic plus retained crypto, statement-state, and user/version blocked-function classes, and index DDL over nondeterministic or session-dependent virtual generated expressions, and errno 1062 side-effect-free failure when replacing stored or virtual generated expressions would violate existing unique generated-column indexes; hook-build crash coverage kills representative failed generated-column `CREATE TABLE`, `ALTER TABLE`, and generated-column primary-key writers after MariaDB validation failure plus successful generated-column `CREATE TABLE`, generated-column `ALTER TABLE ... ADD COLUMN`, generated-column secondary-index, generated-column child/referenced-column `ALTER TABLE ... ADD CONSTRAINT` FK writers after native DDL completion, generated-column child/referenced-column `ALTER TABLE ... DROP FOREIGN KEY` FK writers after native metadata removal but before ownerless dictionary finish, and generated-column FK parent-delete writers immediately before InnoDB executes child-side cascade actions, after a successful child-side cascade returns before parent statement commit, inside `row_upd_step()` before the child-table update/delete is applied, or after one child-side `row_upd()` succeeds in a multi-child cascade, verifying no rejected native metadata leaks, stable retry errno 1901/1903, recovered generated-column metadata, generated values, forced generated-column index reads, generated-column FK enforcement or absence, retryable cascades, ownerless/native reopen, and forced `.shm` rebuild; exhaustive retained-function blocked-function matrices, exhaustive online-option matrices, exhaustive generated-column FK partial child-row crash matrices, and external oracle stress remain planned |
| Ownerless foreign-key action crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills ordinary and generated-column FK action writers before child-side cascade execution, after successful child-side action return before parent statement commit, before a child-side `row_upd()` call inside `row_upd_step()`, and after one child-side `row_upd()` succeeds in a multi-child cascade. Recovery verifies live-peer cleanup remains busy, no-live recovery rolls back parent/child changes, retries succeed, and final ownerless/native reopen before and after forced `.shm` rebuild observes the expected CASCADE and SET NULL state; exhaustive later-row fault selection, graph-wide randomized crash matrices, and long-running external MariaDB/RQG stress remain planned |
| `CREATE TABLE ... LIKE` | 🟡&nbsp;Partial | Representative MariaDB table-definition copy behavior is covered for default-engine tables, including ownerless peer visibility from an already-open handle, hook-build crash recovery after native destination table creation plus `CREATE OR REPLACE TABLE ... LIKE` replacement-copy completion but before ownerless dictionary finish, and no-live stale-reader replay for `CREATE OR REPLACE TABLE ... LIKE` preserving the copied-shape replacement tablespace final state with page-0 space-id identity checks |
| `CREATE TABLE ... SELECT` | 🟡&nbsp;Partial | Representative CTAS behavior is covered over MyLite tables, including ownerless peer visibility from an already-open handle, hook-build crash recovery after native destination table creation/population plus `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy completion but before ownerless dictionary finish, and no-live stale-reader replay for `CREATE OR REPLACE TABLE ... AS SELECT` preserving the populated replacement tablespace final state with page-0 space-id identity checks |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE`, qualified table access, and `DROP DATABASE` lifecycle are covered inside `datadir/`; ownerless coverage verifies peer refresh for a schema and InnoDB table created by another process, schema default charset/collation creation and `ALTER DATABASE` refresh through MariaDB's native `db.opt` file, idempotent `CREATE SCHEMA IF NOT EXISTS` / `CREATE DATABASE IF NOT EXISTS` and `DROP SCHEMA IF EXISTS` spellings including no-op duplicate-create/default-preservation and absent-drop behavior, peer-visible schema drop, final absent-schema state through ownerless/native reopen before and after forced `.shm` rebuild, hook-build crash recovery for killed `CREATE DATABASE` after native schema directory/`db.opt` creation, killed `ALTER DATABASE` after native `db.opt` rewrite, killed duplicate `CREATE DATABASE IF NOT EXISTS` preserving original defaults, killed missing `DROP SCHEMA IF EXISTS` preserving real schema/table state and missing-schema absence, and killed `DROP DATABASE` after native schema/table removal, each before ownerless dictionary finish, and retained-WAL stale-reader schema-drop replay preserving dropped schema and multi-table absence through ownerless/native reopen before and after forced `.shm` rebuild; broader schema behavior remains planned |
| Sequences | 🟡&nbsp;Partial | Simple MariaDB `CREATE SEQUENCE ... NOCACHE`, `NEXT VALUE FOR`, and `DEFAULT NEXTVAL()` behavior is covered across close and reopen in ordinary exclusive embedded mode; ownerless read/write mode rejects sequence DDL, direct and prepared top-level sequence value access before prepared-statement allocation, and hidden sequence expression execution from existing metadata before mutating sequence state; broader sequence DDL, ownerless sequence coordination, and edge cases remain planned |
| Representative application schemas | 🟡&nbsp;Partial | WordPress-shaped InnoDB `wp_options`, `wp_posts`, and `wp_postmeta` DDL and queries are covered as representative application-schema evidence |
| Views, triggers, and routines | 🟡&nbsp;Partial | Minimal `mysql.proc` / `mysql.procs_priv` metadata is initialized inside the MyLite directory, simple result-returning direct stored-procedure create, show, call, and drop behavior is covered, simple view create/query/drop behavior is covered including ownerless peer refresh and no-live ownerless/native reopen after forced `.shm` rebuild over an InnoDB base table, hook-build crash coverage kills simple `CREATE VIEW` and `DROP VIEW` writers after native view definition-file creation/removal plus `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view definition rewrite, explicit column-list create/replace/alter writers after native alias metadata storage/rewrite, check-option create/replacement/alter writers after native check-option metadata storage/rewrite, nested check-option outer-replacement and inner-alter writers after native nested view definition rewrite, explicit definer create writers after native security metadata storage, and invoker replacement writers after native security metadata rewrite but before ownerless dictionary finish, then verifies recovered present/absent, rewritten, column-list, check-option, nested check-option, or security view metadata, `.frm` file state, view query behavior, old exposed-column rejection, base-table writes, and ownerless/native reopen before and after forced `.shm` rebuild, ownerless `CREATE OR REPLACE VIEW` and `ALTER VIEW` definition and explicit column-list refresh are covered for already-open peers with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless view idempotent DDL covers `CREATE VIEW IF NOT EXISTS`, duplicate-create errno 1050, no-op duplicate definition preservation, repeated `DROP VIEW IF EXISTS`, and hook-build crash recovery for duplicate `CREATE VIEW IF NOT EXISTS` plus missing `DROP VIEW IF EXISTS` no-op writers preserving the original view definition and missing-view absence, with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless updatable view `WITH LOCAL/CASCADED CHECK OPTION` metadata refresh and valid/invalid DML through the view are covered for already-open peers with MariaDB errno 1369 on check-option failures and final ownerless/native reopen before and after forced `.shm` rebuild, prepared `SELECT`, `INSERT`, and `UPDATE` through an ownerless check-option view are covered for already-open peers, including MariaDB errno 1369 on invalid prepared insert/update attempts, prepared DML reuse after peer view replacement, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless non-updatable aggregate-view diagnostics cover `IS_UPDATABLE = 'NO'`, errno 1471 for `INSERT`, errno 1288 for `UPDATE`/`DELETE`, errno 1368 for rejected `WITH CHECK OPTION`, direct failed-write immutability, ownerless prepared-DML step-time errno 1471 for prepared `INSERT`, ownerless prepared-DML step-time errno 1288 for prepared `UPDATE`/`DELETE`, peer replacement refresh, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless invalid view dependency diagnostics cover MariaDB errno 1356 after a peer drops the base table, recovery after the peer recreates that base table, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless nested updatable view coverage verifies outer `LOCAL` versus `CASCADED` propagation over an inner check-option view plus inner-view predicate refresh with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless view security/definer coverage verifies `DEFINER=CURRENT_USER`, `SQL SECURITY DEFINER`, and `SQL SECURITY INVOKER` metadata refresh for already-open peers with final ownerless/native reopen before and after forced `.shm` rebuild, simple trigger create/fire/drop behavior is covered with ownerless peer refresh and no-live ownerless/native reopen after forced `.shm` rebuild over InnoDB base/audit tables, ownerless trigger variants cover `BEFORE UPDATE` `NEW` mutation, `CREATE OR REPLACE TRIGGER` replacement, and `AFTER DELETE` `OLD` audit effects through an already-open peer with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless trigger ordering covers `FOLLOWS`/`PRECEDES` `ACTION_ORDER`, firing order, and `SHOW CREATE TRIGGER` lookup by trigger name through an already-open peer with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless trigger idempotent DDL covers `CREATE TRIGGER IF NOT EXISTS`, duplicate-create errno 1359, no-op duplicate preservation, and repeated `DROP TRIGGER IF EXISTS` with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless stored routine DDL (`CREATE`/`ALTER`/`DROP FUNCTION`, `PROCEDURE`, `PACKAGE`, or `PACKAGE BODY`) is rejected before mutating `mysql.proc`, and ownerless direct/prepared `CALL`, direct/prepared stored-function expression execution, and trigger-body stored procedure/function execution are rejected before routine-body effects mutate InnoDB rows; ownerless stored routine DDL support, ownerless routine execution support, view privilege/security semantics, invalid view definers, broader view semantics, broader trigger edge cases, packages, routine edge cases, empty-result metadata, and metadata compatibility remain planned |
| Trigger DDL crash recovery | 🟡&nbsp;Partial | Hook-build crash coverage kills simple `CREATE TRIGGER` and `DROP TRIGGER`, `CREATE OR REPLACE TRIGGER`, ordered `CREATE TRIGGER ... PRECEDES ...`, duplicate `CREATE TRIGGER IF NOT EXISTS`, missing `DROP TRIGGER IF EXISTS`, delayed missing-dependency `CREATE TRIGGER`, explicit `CREATE DEFINER=CURRENT_USER TRIGGER`, and stored-function-body `CREATE TRIGGER` writers after native `.TRG`/`.TRN` metadata creation/removal, rewrite, no-op preservation, delayed dependency acceptance, or definer metadata storage but before ownerless dictionary finish, then verifies live-peer cleanup remains busy until no-live recovery, recovered present/absent `INFORMATION_SCHEMA.TRIGGERS` metadata, native trigger-file state, trigger firing after recovered create, replacement, idempotent no-op preservation, delayed dependency creation, definer recovery, stored-function trigger metadata recovery, ownerless stored-routine execution rejection before base-row mutation, and ordinary native stored-function trigger firing, recovered MariaDB 1146 failure when the missing dependency is absent, recovered `ACTION_ORDER`/firing order after ordered create, non-firing base-table inserts after recovered drop, `SHOW CREATE TRIGGER` for recovered present triggers including explicit definer metadata and rejection for the dropped trigger, ownerless/native reopen before and after forced `.shm` rebuild, and absent missing-trigger `.TRN` state after recovered missing drop; broader privilege/security and randomized trigger crash variants remain planned |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile; event DDL, event metadata commands, and scheduler variables are rejected by direct and prepared policy coverage, including ownerless read/write coverage that verifies rejected event metadata stays absent across ownerless/native reopen before and after forced `.shm` rebuild, and the default embedded archive uses only a parser-link event parse-data stub |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management; account, role, grant, revoke, and password statements are rejected by policy coverage, and the default embedded archive omits the `unix_socket` server auth plugin |
| Foreign-server metadata | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-global remote connection metadata; `CREATE SERVER`, `CREATE OR REPLACE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` are rejected by policy coverage, and the default embedded archive omits the `mysql.servers` metadata cache |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior; replication and binlog command families, SQL `BINLOG` replay, GTID helper functions, GTID state variable assignments, and binary-log GTID-index tuning variables are rejected or omitted, `@@log_bin=0` is covered, and the default embedded archive omits the active binlog transaction/event core, SQL `BINLOG` replay source, server event writers, binary-log event parser/reader runtime, replication GTID-state runtime, binary-log GTID-index runtime, residual replication helper objects, unsupported injector root, guarded replication execution system variables, and replication/binlog filter runtime |
| External XA transactions | ➖&nbsp;Out&nbsp;of&nbsp;scope | Distributed transaction-manager surface; direct and prepared `XA` statements are rejected by policy coverage, and the default embedded archive omits the external-XA runtime plus the mmap-backed `tc.log` transaction coordinator while ordinary native-engine transactions remain covered |
| SQL `HANDLER` commands | ➖&nbsp;Out&nbsp;of&nbsp;scope | Low-level server table-cursor surface; direct and prepared top-level `HANDLER ...` statements are rejected by policy coverage, and the default embedded archive omits SQL `HANDLER` command runtime while retaining MariaDB's storage-engine `handler` abstraction |
| Host-file SQL exports | ➖&nbsp;Out&nbsp;of&nbsp;scope | `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` write arbitrary host files outside result delivery; direct and prepared forms are rejected by policy coverage, and the default embedded archive omits the host-file writer bodies while retaining `SELECT ... INTO` variables |
| Host-file SQL imports | ➖&nbsp;Out&nbsp;of&nbsp;scope | `LOAD DATA` and `LOAD XML` read arbitrary host files or client-protocol file streams outside the `libmylite` parameter API; direct and prepared forms are rejected by policy coverage, ownerless retained-WAL pressure coverage verifies those diagnostics are not masked by `MYLITE_BUSY`, and the default embedded archive omits the import runtime while retaining ordinary `INSERT`, prepared bindings, and `INSERT ... SELECT` |
| Dynamic plugin installation | ➖&nbsp;Out&nbsp;of&nbsp;scope | The embedded core uses a transient database-local plugin directory, rejects `INSTALL PLUGIN` / `UNINSTALL PLUGIN` through policy coverage, reports `@@have_dynamic_loading=NO`, and omits runtime shared-object plugin loading from the default archive |
| Dynamic UDF registration | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-owned shared-library loading; `CREATE FUNCTION ... SONAME` and aggregate UDF registration are rejected by policy and the default embedded archive omits the UDF runtime |
| VIO TLS transport | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` opens a local database directory without a socket or TLS handshake; the default embedded archive omits MariaDB's VIO TLS transport, inherited `mysql_ssl_set()` calls fail closed, and first-party linked artifacts no longer depend on `libssl`, while SQL crypto functions retain `libcrypto` |
| Network client authentication handshake | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` opens a local database directory without client/server auth-plugin negotiation; the default embedded archive omits inherited client auth plugin descriptors and plugin VIO handshake helpers, while raw remote client auth and `mysql_change_user()` fail closed |
| PROXY protocol listener | ➖&nbsp;Out&nbsp;of&nbsp;scope | Core `libmylite` has no socket listener or network handshake; the default embedded archive omits MariaDB's PROXY protocol parser and `proxy_protocol_networks` system variable |
| Oracle SQL mode | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional MariaDB compatibility mode, not core MySQL/MariaDB application behavior; attempts to set `sql_mode=ORACLE` are rejected and the embedded archive links an unsupported parser stub |
| Oracle compatibility function aliases | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional Oracle migration aliases such as `DECODE_ORACLE`, `LPAD_ORACLE`, and `oracle_schema` routing are omitted from the default embedded archive; ordinary MySQL/MariaDB string functions remain covered |
| SQL `HELP` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server help-table lookup depends on `mysql.*` help tables and is rejected by policy coverage |
| Statement profiling | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; `@@have_profiling=NO` is covered, profiling commands, variables, and `INFORMATION_SCHEMA.PROFILING` reads are rejected by policy coverage, and the default embedded archive omits the remaining profiling metadata source |
| Query cache | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-side result-cache optimization; `@@have_query_cache=NO` is covered, management commands and variables are rejected, and `SQL_CACHE` / `SQL_NO_CACHE` remain accepted no-op hints |
| Optimizer trace | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic surface; optimizer-trace variables and `INFORMATION_SCHEMA.OPTIMIZER_TRACE` reads, including unqualified reads while `information_schema` is current, are rejected by policy and omitted from the default embedded archive while ordinary planning, execution, and `EXPLAIN` remain supported |
| Persistent optimizer statistics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-owned `mysql.*` optimizer-statistics metadata; the default embedded profile starts with `@@use_stat_tables=NEVER` and `@@histogram_size=0`, rejects persistent `ANALYZE TABLE ... PERSISTENT FOR ...` and statistic variable changes, and omits persistent statistics plus JSON histogram storage while ordinary `ANALYZE TABLE`, engine estimates, planning, and `EXPLAIN` remain supported |
| General and slow query logs | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon query-audit diagnostics; query-log variables and log flush commands are rejected by policy, `@@general_log=0`, `@@slow_query_log=0`, and `@@log_output=NONE` are covered, and the default embedded archive omits query-log handlers while error logging and SQL diagnostics remain available |
| Statement digest diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Performance Schema diagnostic surface; the default embedded archive omits statement digest normalization, `@@max_digest_length=0` is covered, and ordinary parsing, execution, prepared statements, diagnostics, and `EXPLAIN` remain supported |
| Server status variables | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server diagnostic counters; the default embedded archive omits status-variable publication, `SHOW STATUS` and status Information Schema tables return empty rows, and ordinary SQL diagnostics, warnings, result metadata, and the public API remain available |
| Process-list metadata | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon thread/session inventory; `SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` are rejected, `INFORMATION_SCHEMA.PROCESSLIST` returns zero rows, and the default embedded archive omits the process-list row producers |
| Thread-control SQL | ➖&nbsp;Out&nbsp;of&nbsp;scope | Daemon connection and lifetime control; direct and prepared `KILL` and `SHUTDOWN` forms are rejected by server-surface policy, including executable-comment `KILL`, while ordinary string literals containing those words remain valid |
| User statistics diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional server diagnostic counters; the default embedded archive omits the `userstat` plugin and system variable, rejects userstat Information Schema tables and `FLUSH *_STATISTICS`, and keeps ordinary application tables with the same names usable outside `information_schema` |
| User-variable diagnostics | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional session introspection/reset surface; the default embedded archive omits the `user_variables` plugin, rejects `INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW USER_VARIABLES`, and `FLUSH USER_VARIABLES`, and keeps ordinary `@variable` SQL plus application tables named `user_variables` usable |
| External backup runtime | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server backup-tool coordination surface; `BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are rejected by policy coverage, and the default embedded archive omits the active backup runtime while keeping ordinary DDL hooks inert |
| Server utility SQL functions | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server benchmarking, named locks, host-file reads, replication waits, sleeping, and server-id based ID generation; direct and prepared `BENCHMARK()`, `GET_LOCK()` and related helpers, `LOAD_FILE()`, replication wait/position helpers, `SLEEP()`, and `UUID_SHORT()` are rejected by policy and omitted from the default embedded archive, while ordinary scalar functions, JSON, GEOMETRY/GIS, DDL/DML, transactions, and native storage remain supported |
| Vector SQL runtime | ➖&nbsp;Out&nbsp;of&nbsp;scope | MariaDB vector conversion, distance, and MHNSW vector-index runtime are not part of the current embedded profile; direct and prepared `VEC_FROMTEXT()`, `VEC_TOTEXT()`, `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and `VEC_DISTANCE_COSINE()` calls are rejected by policy, vector-index DDL is covered as rejected, and the default embedded archive omits `item_vectorfunc.cc`, `vector_mhnsw.cc`, and mandatory `mhnsw` plugin registration while retaining `VECTOR(N)` type parsing as a separate compatibility decision |
| XML SQL helpers | ➖&nbsp;Out&nbsp;of&nbsp;scope | Legacy XPath helper functions; direct and prepared `EXTRACTVALUE()` and `UPDATEXML()` calls are rejected by policy and omitted from the default embedded archive, while ordinary SQL, JSON, GEOMETRY/GIS, native storage, and the separately unsupported `LOAD XML` host-file import boundary remain unchanged |
| Dynamic columns | ➖&nbsp;Out&nbsp;of&nbsp;scope | MariaDB-specific dynamic-column SQL helpers; direct and prepared `COLUMN_CREATE()`, `COLUMN_ADD()`, `COLUMN_DELETE()`, `COLUMN_GET()`, `COLUMN_CHECK()`, `COLUMN_EXISTS()`, `COLUMN_LIST()`, and `COLUMN_JSON()` calls are rejected by policy and the default embedded archive keeps only fail-closed dynamic-column C helper stubs, while ordinary SQL, JSON, GEOMETRY/GIS, native storage, and result metadata remain unchanged |
| `SFORMAT()` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Optional MariaDB fmtlib-backed formatting helper; omitted from the embedded profile so the embedded SQL target can build without C++ exceptions, while ordinary `FORMAT()` remains available |
| `PROCEDURE ANALYSE()` | ➖&nbsp;Out&nbsp;of&nbsp;scope | Legacy diagnostic SELECT extension; rejected by policy and omitted from the default embedded archive while ordinary SELECT queries remain supported |
| System-variable help comments | 🟡&nbsp;Partial | `SHOW VARIABLES` and system-variable rows, values, defaults, and validation remain available; `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default embedded profile to omit server help text |
| Static `SHOW` information | ➖&nbsp;Out&nbsp;of&nbsp;scope | `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` expose static server attribution and privilege-help metadata; they are rejected by policy and omitted from the default embedded archive while ordinary supported `SHOW` surfaces remain available |

## Rows, Indexes, And Constraints

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Fixed and variable row fields | 🟡&nbsp;Partial | Controlled MyISAM rows cover integer, variable string, `TEXT`, and `BLOB` storage across update, delete, close, and reopen |
| NULL columns | 🟡&nbsp;Partial | Controlled MyISAM nullable unique-key values are covered; broader NULL comparison and type matrix coverage remains planned |
| BLOB/TEXT values | 🟡&nbsp;Partial | Controlled MyISAM `TEXT` and `BLOB` values are stored, updated, and read through SQL expressions; prepared blob coverage verifies binary-safe values with embedded NUL bytes |
| Primary and secondary indexes | 🟡&nbsp;Partial | Controlled MyISAM primary and secondary indexed predicates are covered, including an index added by copy-style `ALTER TABLE`; ownerless InnoDB coverage verifies peer refresh for secondary-index create/drop/rename, descending and mixed-direction key-part direction, prefix-plus-direction key-part direction/length, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix key-part length, TEXT/BLOB prefix-plus-direction key-part length/direction, unique TEXT/BLOB prefix length/enforcement, unique TEXT/BLOB prefix-plus-direction length/direction/enforcement, and ignored/not-ignored metadata, unique secondary-index enforcement/drop, primary-key idempotent ADD no-op preservation including hook-build crash recovery for the no-op branch and replacement from `id` to `code` with duplicate enforcement on the replacement key, descending primary-key replacement metadata/enforcement, composite direction primary-key replacement metadata/enforcement, AUTO_INCREMENT primary-key replacement that keeps the AUTO_INCREMENT column as a unique secondary index, and AUTO_INCREMENT descending primary-key replacement metadata/allocation |
| Unique indexes | 🟡&nbsp;Partial | Controlled MyISAM duplicate-key diagnostics and nullable unique-key inserts are covered; ownerless InnoDB coverage verifies multi-column unique secondary-index enforcement/drop, unique descending secondary-index direction metadata, unique prefix secondary-index length metadata, unique prefix-plus-direction secondary-index length/direction metadata, utf8mb4 prefix character-count length metadata, unique TEXT/BLOB prefix length metadata, unique TEXT/BLOB prefix-plus-direction length/direction metadata, duplicate rejection while present, and duplicate insertion after drop |
| Autoincrement | 🟡&nbsp;Partial | Controlled MyISAM table-local autoincrement state is covered across close and reopen; ownerless InnoDB implicit AUTO_INCREMENT inserts reserve distinct values across concurrently opened writer processes through a directory-backed shared high-watermark registry, ownerless `ALTER TABLE ... AUTO_INCREMENT` high-watermark refresh is covered for an already-open peer plus ownerless/native reopen before and after forced `.shm` rebuild, hook-build crash recovery kills `ALTER TABLE ... AUTO_INCREMENT` after native high-watermark persistence but before ownerless dictionary finish and verifies recovered monotonic implicit IDs through ownerless/native reopen before and after forced `.shm` rebuild, rebuild-style ownerless `ALTER TABLE ... ADD COLUMN ... AUTO_INCREMENT PRIMARY KEY` refresh is covered for an already-open peer plus ownerless/native reopen before and after forced `.shm` rebuild, and ownerless primary-key replacement on an existing AUTO_INCREMENT column, including a descending replacement key, preserves the unique secondary key and no-reuse gap after duplicate-key failure |
| CHECK constraints | 🟡&nbsp;Partial | Representative default-engine CHECK constraint metadata and enforcement are covered through MariaDB expression evaluation; ownerless coverage verifies peer refresh for table-level `ALTER TABLE` CHECK constraint add/drop with errno 4025 enforcement before drop and formerly invalid row insertion after drop, plus peer refresh for column-level CHECK metadata and generated-column CHECK enforcement with formerly invalid rows accepted after both constraints are dropped, and hook-build crash coverage kills CHECK ADD/DROP writers after native table-definition mutation but before ownerless dictionary finish, then verifies recovered CHECK metadata/enforcement for ADD, recovered column-level CHECK metadata plus generated-column CHECK enforcement for an ADD variant, recovered absent CHECK metadata plus formerly invalid writes for DROP, and recovered absent column-level/generated-column CHECK metadata plus formerly invalid writes for a DROP variant |
| Foreign keys | 🟡&nbsp;Partial | Representative InnoDB foreign-key enforcement and cascade delete behavior are covered; ownerless coverage verifies peer refresh for create-time foreign keys plus `ALTER TABLE` foreign-key add/drop with missing-parent enforcement before drop and orphan insertion after drop, cross-process referential actions for `ON UPDATE CASCADE`, `ON DELETE CASCADE`, `ON DELETE SET NULL`, and `ON DELETE RESTRICT`, native FK checks now prepare ownerless current-read visibility and reopen parent/child FK cursors so peer-created child rows and clustered child records are resolved without SQL-level metadata probes, hook-build crash recovery for ordinary parent update/delete writers killed immediately before InnoDB executes child referential actions, after one successful ordinary child action returns before parent statement commit, inside `row_upd_step()` before ordinary child update/delete application, and after one ordinary child-side `row_upd()` succeeds in a multi-child cascade, plus generated-column FK parent-delete writers killed immediately before InnoDB executes child referential actions, after a successful generated-column child cascade returns before parent statement commit, inside `row_upd_step()` before child update/delete application, and after one generated-column child-side `row_upd()` succeeds in a multi-child cascade, tenant-scoped composite foreign-key enforcement/cascade/restrict behavior, multi-hop `ON UPDATE`/`ON DELETE CASCADE` chains through four InnoDB tables, supported stored generated-column child and referenced-column foreign-key shapes plus indexed virtual generated child create/alter shapes with `ON UPDATE RESTRICT`/`ON DELETE CASCADE`, MariaDB-rejected generated-column action clauses with errno 1905, hook-build crash recovery for generated-column child and referenced-column `ALTER TABLE ... ADD CONSTRAINT` and `ALTER TABLE ... DROP FOREIGN KEY` FK writers killed after native metadata creation/removal but before ownerless dictionary finish, two-table and three-table cyclic `ON DELETE CASCADE`, cyclic `ON DELETE SET NULL`, and MariaDB-native cyclic update rejection, same-schema parent-table `RENAME TABLE` metadata refresh, child-table `RENAME TABLE` refresh for generated `<child>_ibfk_1` constraint names, cross-schema parent-table `RENAME TABLE` refresh with `UNIQUE_CONSTRAINT_SCHEMA` movement, cross-schema child-table `RENAME TABLE` refresh with `CONSTRAINT_SCHEMA` and generated `<child>_ibfk_1` movement, same-schema plus cross-schema multi-pair parent/child `RENAME TABLE` refresh with valid-child insertion, missing-parent rejection, restricted-delete rejection, and ownerless/native reopen before and after forced `.shm` rebuild, and opt-in deterministic ownerless foreign-key graph stress over concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers with bounded retry for MariaDB 1205/1213, transient page-write commit-boundary coverage plus transaction page LSN coverage and DML current-read refresh for parent clustered/secondary index pages, errno 1451/1452 checks, ownerless/native reopen before and after forced `.shm` rebuild, deterministic SQL trace export for external harness input, and seeded FK graph trace-suite validation for deterministic external-oracle variants; full external MariaDB/RQG FK graph execution and randomized later-child-row intra-action FK graph crash edge cases remain planned |
| Ownerless FK graph external replay | 🟡&nbsp;Partial | `tools/ownerless-fk-graph-trace` emits bounded `1205`/`1213` plus SQLSTATE `40001` retry procedures around each deterministic FK graph worker transaction so a raw external `mariadb` client can replay concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers through the Docker-backed trace smoke; seed `0` preserves the default deterministic graph schedule, nonzero seeds generate deterministic external-oracle delta variants, `tools/ownerless-fk-graph-seed-suite` validates or replays multiple seeded traces through the common trace runner with bounded whole-seed replay attempts for transient external `1205`/`1213` escapes, and `tools/ownerless-external-mariadb-seed-sweep` now includes FK graph in its combined check/replay plan; Docker-backed MariaDB 11.8 replay passed seeds `0`, `17`, `83`, and `211` at rounds `2`, with seed `17` recovering on attempt `2` and seed `83` recovering on attempt `3`; long-running randomized MariaDB/RQG FK graph execution and deeper intra-action FK graph crash coverage remain planned |
| Ownerless random transaction trace export | 🟡&nbsp;Partial | `tools/ownerless-random-tx-trace` emits the deterministic ownerless random transaction schedule as schema, per-worker SQL, manifest metadata, and final aggregate oracles; seed `0` preserves the product C stress formulas, while nonzero seeds generate deterministic external-oracle variants for row choice, rollback points, and update deltas. `tools/ownerless-random-tx-seed-suite` validates or replays multiple seeded traces through the common trace runner, with dependency-free CTest coverage and focused Docker-backed MariaDB 11.8 replay evidence for seeds `0`, `17`, `83`, and `211`; long-running randomized MariaDB/RQG transaction generation remains planned |
| Generated columns | 🟡&nbsp;Partial | Representative stored and virtual generated-column behavior is covered through native storage support; ownerless coverage verifies peer refresh for create-time generated columns plus `ALTER TABLE` generated-column add/drop, same-kind stored/virtual expression replacement, and indexed same-kind expression replacement with generated expression reads, forced-index reads, base-column writes, and reopen checks; ownerless policy coverage preserves MariaDB's errno 1901 rejection for representative nondeterministic stored generated expressions plus aggregate, subquery, time-dependent, session-dependent, nondeterministic, retained crypto, statement-state, and user/version blocked-function classes, and hook-build crash coverage proves representative failed generated-column DDL leaves clean metadata after recovery |
| Generated-column indexes | 🟡&nbsp;Partial | Ownerless coverage verifies stored and virtual generated-column secondary-index create/use/drop refresh, unique generated-column indexes, prefix generated-column indexes, mixed-direction composite generated-column indexes, accepted explicit `NOCOPY`/`INPLACE` generated-column index add/drop options, forced-index reads, generated-value recalculation after base-column DML, and indexed generated-expression replacement for ordinary stored and virtual generated-column secondary indexes; ownerless policy coverage preserves MariaDB's generated-column primary-key rejection with errno 1903, generated-column-function rejection with errno 1901 for indexes over nondeterministic and session-dependent virtual generated expressions, and duplicate-key rejection with errno 1062 for generated-expression replacement that would violate existing unique generated-column indexes; representative failed and successful generated-column DDL crash recovery plus pre-child-action, post-child-action, row-step-before-update, and first child-row after-update generated-column FK action crash recovery are covered, while exhaustive retained-function blocked-function, exhaustive online-option, later-row generated-column FK partial child-row crash, and external-oracle matrices remain planned |
| FULLTEXT, SPATIAL, and vector indexes | ⚪&nbsp;Planned | Support only where the selected native engine and embedded profile support them; ownerless read/write mode rejects top-level, ALTER, inline, and idempotent `FULLTEXT` and `SPATIAL` index DDL until full-text auxiliary state, spatial R-tree pages, and spatial predicate-lock coordination are designed, and vector index DDL is rejected by the default no-vector profile |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Explicit InnoDB transactions commit through native MariaDB/InnoDB hooks inside the MyLite database directory; ownerless coverage pauses multiple independent-table transactions before commit, releases the commits together, and verifies every delta is durable |
| Rollback | 🟡&nbsp;Partial | Explicit InnoDB transaction rollback is covered; MyISAM remains non-transactional |
| Savepoints | 🟡&nbsp;Partial | Explicit InnoDB savepoint rollback and release savepoint are covered through SQL transaction statements |
| Crash recovery | 🟡&nbsp;Partial | Parent-process reopen after child-process exit covers committed InnoDB rows surviving and uncommitted rows rolling back |
| Same-process multi-handle concurrency | 🟡&nbsp;Partial | Multiple `mylite_db` handles over one embedded runtime are covered for committed visibility, simultaneous active InnoDB transactions on different rows, row-lock timeout behavior, shared wait-edge publication for local InnoDB row waits, metadata-lock timeout behavior, savepoints, and foreign-key enforcement |
| Multiple readers | 🟡&nbsp;Partial | Ownerless read/write opens and `MYLITE_OPEN_READONLY \| MYLITE_OPEN_SHARED_READONLY` handles can read committed InnoDB updates from peer processes through native-file refresh and safe page-version reads, including prepared `SELECT` execution, shared read-only repeatable-read snapshots while a peer ownerless writer commits, tested read-only transaction first-read/repeatable-snapshot behavior, `READ COMMITTED` transaction reads that observe a later peer commit, and reads inside transactions after local writes; ownerless `READ UNCOMMITTED` isolation requests are rejected before unproven cross-process dirty reads; stale ownerless coordination that needs recovery must be reopened read/write before shared read-only handles can attach |
| Concurrent writers | 🟡&nbsp;Partial | Ownerless cross-process read/write opens coordinate InnoDB row/table locks, gap/next-key locks that block peer inserts and allow post-release retry, serializable read locks that block peer writers, a serializable write-skew candidate where two predicate readers cannot both commit disjoint updates, transaction and savepoint-rollback visibility, concurrent explicit commit visibility through live ownerless opens, forced shared-memory rebuild, and native exclusive reopen for the covered explicit-commit race, DDL/DML stress, temporary-table stress, checksum-oracle stress, explicit transaction/savepoint stress, pseudo-random transaction stress, and foreign-key graph stress shapes, redo visibility, targeted post-wait page refresh, representative metadata-lock blocking, concurrent DDL table/space/index metadata allocation including online index replacement, peer-refresh visibility for foreign-key, generated-column, idempotent table create/drop, `CREATE TABLE ... LIKE`, CTAS, online/in-place index DDL, table charset conversion, row-format rebuild, table comment metadata, `ALTER TABLE ... FORCE` rebuild, column default set/drop, column idempotent add/drop, standalone `CREATE INDEX`/`DROP INDEX` including idempotent secondary-index create/drop and ordinary inline `CREATE TABLE ... INDEX` create/duplicate-failure semantics, multi-column, unique descending, unique prefix, unique prefix-plus-direction, utf8mb4 prefix, unique TEXT/BLOB prefix, and unique TEXT/BLOB prefix-plus-direction secondary-index enforcement before drop, and descending, mixed-direction, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix, TEXT/BLOB prefix-plus-direction, and prefix-plus-direction key-part metadata, secondary-index rename and ignored/not-ignored metadata, primary-key idempotent ADD no-op preservation plus replacement with duplicate enforcement on the new key, descending primary-key replacement direction metadata and duplicate enforcement, composite direction primary-key replacement metadata and duplicate enforcement, AUTO_INCREMENT descending primary-key replacement metadata and duplicate allocation gap enforcement, foreign-key ALTER add/drop enforcement, cross-process foreign-key referential actions, composite foreign-key enforcement/cascade/restrict behavior, deep foreign-key cascade-chain update/delete behavior, stored/virtual generated-column foreign-key enforcement/restrict/cascade behavior plus MariaDB-rejected generated-column action policy, cyclic foreign-key cascade/set-null/rejected-update behavior, same-schema foreign-key parent-table/child-table rename metadata/enforcement, cross-schema foreign-key parent-table/child-table rename metadata/enforcement, same-schema and cross-schema foreign-key multi-pair parent/child rename metadata/enforcement, CHECK constraint ALTER add/drop enforcement, generated-column ALTER add/drop and same-kind expression-replacement refresh, column add/modify/rename/drop ALTERs, explicit InnoDB instant ADD/DROP/reorder column metadata plus instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants refresh, schema create/drop lifecycle refresh, schema default charset/collation DDL refresh plus schema-default rewrite crash recovery, cross-schema InnoDB table rename and multi-pair rename-cycle refresh with ownerless/native reopen before and after forced `.shm` rebuild, view create/query/drop, replacement/alter, idempotent create/drop, column-list, check-option, prepared check-option DML, direct/prepared non-updatable view diagnostics, invalid view dependency diagnostics, nested check-option, and security/definer metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, trigger create/fire/drop, replacement/update/delete, ordering/show-create, and idempotent create/drop metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, ownerless stored routine DDL rejection before uncoordinated `mysql.proc` writes, including MariaDB package specification/body routine rows, and ownerless top-level `CALL`, stored-function expression, and trigger-body procedure/function execution rejection before routine-body effects mutate InnoDB rows, ownerless top-level sequence SQL plus hidden `DEFAULT NEXTVAL()` execution rejection before uncoordinated sequence-table writes, ownerless table-admin SQL rejection before uncoordinated checksum reads, statistics, upgrade-check, repair, or admin-rebuild paths, ownerless `LOCK TABLES` rejection before unproven SQL locked-table mode, ownerless `FLUSH TABLES ... WITH READ LOCK`/`FOR EXPORT` rejection before unproven global read-lock, locked-table, and export/quiesce paths, ownerless `READ UNCOMMITTED` isolation rejection before unproven cross-process dirty reads, ownerless table `DATA DIRECTORY`/`INDEX DIRECTORY` option rejection before external file lifecycle paths, ownerless top-level, ALTER, inline, and idempotent `FULLTEXT`/`SPATIAL` index DDL rejection before unproven special-index storage writes, ownerless partitioned table DDL rejection before unproven partition file-lifecycle writes, no-live ownerless/native exclusive reopen of the final broader DDL state before and after forced `.shm` rebuild, no-live stale-reader rebuild discard for retained reader-boundary WAL covering single-table dropped, same-schema and cross-schema same-statement multi-dropped, ordinary-created, LIKE-copy, CTAS-created, recreated, renamed, truncated, force-rebuilt, and multi-rename-swap file-per-table final states plus multi-table schema-drop absence, dirty no-live recovery skip for retained WAL records whose dropped file-per-table tablespace no longer exists, and large-table reuse after peer truncate, local DDL readability after dictionary flush, concurrent same-named InnoDB temporary tables, killed temporary-table peer cleanup while another temp-table peer remains live, shared AUTO_INCREMENT reservation across concurrent ownerless insert workers, ownerless AUTO_INCREMENT DDL high-watermark refresh for already-open peers, ownerless AUTO_INCREMENT column-add rebuild refresh for already-open peers, multi-object reader/writer stress, opt-in high-pressure ownerless independent-table stress plus deterministic independent-table stress SQL trace export for external harness input, opt-in DDL/DML stress plus deterministic DDL stress and DDL lifecycle SQL trace export for external harness input, opt-in temporary-table stress plus deterministic temporary-table stress SQL trace export for external harness input, opt-in explicit transaction/savepoint stress plus deterministic transaction stress SQL trace export for external harness input, opt-in checksum-oracle stress over one shared table plus deterministic checksum stress SQL trace export for external harness input, opt-in pseudo-random shared-table transaction stress with bounded retry after MariaDB lock-wait/deadlock errors plus a deterministic SQL trace exporter for external oracle runners, opt-in foreign-key graph stress with bounded retry after MariaDB lock-wait/deadlock errors while concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers mutate one graph plus transient page-write commit-boundary, transaction page LSN coverage, and DML current-read refresh plus a deterministic FK graph SQL trace exporter for external harness input, opt-in active-reader pressure stress under a repeatable-read snapshot pin plus deterministic active-reader pressure SQL trace export for external harness input, opt-in ownerless page-version WAL pressure limits for active-reader pins across direct/prepared writes, representative DML/DDL write classes, AUTO_INCREMENT DDL high-watermark ALTER, variant DML/index/rename/truncate spellings, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants, safe page-version reads inside mutating transactions, no-live-process page-version replay to existing tablespaces, retained page-version WAL for native exclusive reopen, no-peer and zero-active-pin live-peer native checkpoint reclamation including dead snapshot-pin cleanup and partial compaction while newer records remain, native boundary synthesis for active readers with product WAL retention until pins release, plus primitive active-pin page-version compaction evidence, native boundary synthesis when an older on-disk page image is still readable at page-version publish time, active snapshot-pin blocking of product live-peer prefix compaction until release when synthesis cannot prove a boundary, thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling when no peer process is live plus live-peer gating coverage before close-time reclaim, timer-driven checkpoint scheduling after shared read-only snapshot release without another writer SQL statement, bounded repeated same-row writer pressure while a live reader pins its snapshot, and conservative same-page write serialization through directory-owned files; external SQL trace replay harnessing is covered for generated deterministic traces plus full-suite trace-runner validation, optional client replay, opt-in disposable MariaDB Docker smoke-safe subset tooling, and full scale-2 deterministic Docker-backed MariaDB replay, while full external MariaDB/RQG long-running stress remains planned |
| Cross-process unsafe writers | 🟡&nbsp;Partial | A second ordinary read/write process open is rejected with `MYLITE_BUSY` while another process owns the MyLite directory lock; ordinary exclusive opens still create fixed ownerless coordination headers but fresh ordinary opens stay on the native MariaDB SQL hot path without installing ownerless runtime lifecycle, MDL, transaction, read-view, or InnoDB hooks, and without appending ownerless page-version WAL payloads unless the handle is opened through `MYLITE_OPEN_OWNERLESS_RW`, `MYLITE_OPEN_SHARED_READONLY`, or retained ownerless WAL payload requires native exclusive replay |
| Ownerless lock fault coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills a blocked writer after it enters the external ownerless record wait but before MariaDB grants the local waiting record lock, and kills a blocked writer after MariaDB grants a waited record lock and MyLite publishes that granted lock to shared state, proving live-peer cleanup remains busy and no-live reopen preserves only committed data. Primitive coverage kills a process while a table-lock wait entry is live, verifies the wait entry remains observable after process death, and proves owner cleanup removes it; hook-build SQL coverage now proves a `foreign_key_checks=0` and `unique_checks=0` empty-table bulk insert waiting behind a peer ownerless `LOCK IN SHARE MODE` reader publishes a shared external native table-wait registry entry, clears it after release, supports retry insert, and remains visible through ownerless/native reopen and forced `.shm` rebuild. Hook-build SQL negative proof still arms the local ownerless table-wait callback while representative blocked `ALTER TABLE`, CHECK/FK add, `CREATE INDEX`, online index add/drop, existing-index drop/rename/ignored, copy-force `ALTER TABLE`, charset conversion, row-format ALTER, `TRUNCATE TABLE`, `RENAME TABLE`, `DROP TABLE`, `CREATE OR REPLACE TABLE ... LIKE`, and `CREATE OR REPLACE TABLE ... AS SELECT` variants time out, verifies blocked metadata remains unchanged, and fails if any tested SQL shape reaches the local callback. SQL-level table-lock wait fault injection remains planned beyond the covered external native table-wait registry path, while ownerless `LOCK TABLES`/`UNLOCK TABLES` is explicitly rejected until SQL locked-table mode is designed |
| Ownerless writer tests | 🟡&nbsp;Partial | Normal embedded builds run `MYLITE_OPEN_OWNERLESS_RW` cross-process SQL writer tests that bypass the process-wide directory lock and route through the ownerless process, transaction, read-view, InnoDB lock, redo-visibility, page-version WAL, and checkpoint hooks; tests cover non-conflicting writers, same-page writer serialization, concurrent explicit commits on independent tables, native exclusive reopen and recovery-anchor checks after the concurrent explicit-commit race before and after forced `.shm` rebuild, row waits, gap-lock insert blocking plus post-release retry, serializable reader/writer blocking, serializable write-skew prevention for disjoint predicate-dependent updates, savepoint rollback before peer-visible commit, deadlocks across separate tables, mixed readers/writers, shared AUTO_INCREMENT assignment across concurrently opened insert workers, ownerless AUTO_INCREMENT DDL high-watermark refresh, ownerless AUTO_INCREMENT column-add rebuild refresh, ownerless AUTO_INCREMENT primary-key replacement refresh, ownerless AUTO_INCREMENT descending primary-key replacement refresh, a bounded independent-table writer/multi-object reader stress loop, committed external visibility through native refresh, direct and prepared SELECT visibility, shared read-only prepared reads and read-only rejection for direct/prepared writes, transaction first-read visibility after peer commits, transaction reads after local writes, repeatable-read and `WITH CONSISTENT SNAPSHOT` retention across peer churn, session-scoped and transaction-scoped read-committed visibility of later peer commits, shared-memory rebuild, no-live-process page-version replay with retained WAL, no-live stale-reader rebuild over retained reader-boundary WAL for single-table dropped, same-schema and cross-schema same-statement multi-dropped, ordinary-created, LIKE-copy, CTAS-created, recreated, renamed, truncated, force-rebuilt, and multi-rename-swap file-per-table tablespaces plus multi-table schema-drop absence, live idle-peer page-log reclamation, active snapshot-pin blocking of live-peer reclamation when boundary proof is missing, native boundary synthesis for a live snapshot pin, active-reader pressure limit throttling for direct/prepared writes including prepared `INSERT ... SELECT`, representative DML/DDL write classes, AUTO_INCREMENT DDL high-watermark ALTER, variant DML/index/rename/truncate spellings, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants, active-reader pressure diagnostics, active-pin boundary retention until release, killed snapshot-pin cleanup allowing live-peer reclamation, cross-process `ALTER TABLE` waiting, concurrent DDL table/space/index metadata allocation with online index replacement, peer-visible ownerless DDL for create, rename, truncate, post-truncate DML, large-table truncate reuse, drop, same-name recreate, idempotent table create/drop, `CREATE TABLE ... LIKE`, CTAS, standalone `CREATE INDEX`/`DROP INDEX` plus idempotent standalone, `ALTER TABLE`, and inline `CREATE TABLE` index create/duplicate-failure semantics, descending, mixed-direction, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix, TEXT/BLOB prefix-plus-direction, and prefix-plus-direction key-part metadata refresh, secondary-index rename, secondary-index ignored/not-ignored metadata refresh, multi-column unique replacement/enforcement plus idempotent top-level and `ALTER TABLE` unique-index create/no-op/drop preservation, unique descending, unique prefix, unique prefix-plus-direction, utf8mb4 prefix, unique TEXT/BLOB prefix, and unique TEXT/BLOB prefix-plus-direction secondary-index DDL enforcement/drop refresh, primary-key idempotent ADD no-op preservation, primary-key, descending-primary-key, and composite direction primary-key replacement DDL refresh, foreign-key ALTER add/drop refresh, foreign-key referential actions, composite foreign-key coverage, deep foreign-key cascade-chain update/delete coverage, generated-column foreign-key coverage including virtual generated child and rejected action policy, cyclic foreign-key coverage including three-table cascade and set-null variants, same-schema foreign-key parent-table/child-table rename refresh, cross-schema foreign-key parent-table/child-table rename refresh, same-schema and cross-schema foreign-key multi-pair parent/child rename refresh, CHECK constraint ALTER add/drop enforcement, generated-column ALTER add/drop and same-kind expression-replacement refresh, table charset-conversion DDL refresh, row-format DDL refresh, table-comment DDL refresh, force-rebuild DDL refresh, column-default SET/DROP refresh, column idempotent ADD/DROP refresh, column add-modify-rename-drop ALTERs, explicit instant ADD/DROP/reorder column metadata, and instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants refresh, schema create/drop lifecycle refresh, schema default charset/collation DDL refresh plus schema-default rewrite crash recovery, and no-live absent-schema reopen checks, cross-schema table rename with `.frm`/`.ibd` movement, multi-pair rename-cycle tablespace swap, and ownerless/native reopen before and after forced `.shm` rebuild, view create/query/drop, replacement/alter, idempotent create/drop, column-list, check-option, prepared check-option DML, direct/prepared non-updatable view diagnostics, invalid view dependency diagnostics, nested check-option, and security/definer metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, trigger create/fire/drop, replacement/update/delete, ordering/show-create, and idempotent create/drop metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, no-live ownerless/native exclusive reopen of the broader DDL final state before and after forced `.shm` rebuild, local DDL followed by dictionary/table flush, foreign keys, CHECK constraints, generated columns, online/in-place index alter, concurrent same-named InnoDB temporary tables, killed temporary-table peer cleanup while another temp-table peer remains live, ownerless InnoDB-only engine policy, ownerless stored-routine DDL including package specification/body and routine-execution policy rejection, ownerless sequence SQL policy rejection, ownerless table-admin SQL policy rejection, ownerless `LOCK TABLES` SQL policy rejection, ownerless `FLUSH TABLES ... WITH READ LOCK`/`FOR EXPORT` SQL policy rejection while plain ownerless `FLUSH TABLES` remains covered, ownerless table-directory SQL policy rejection, ownerless top-level, ALTER, inline, and idempotent `FULLTEXT`/`SPATIAL` index DDL policy rejection, ownerless partitioned table DDL policy rejection, and a killed uncommitted writer whose state blocks peer cleanup until a no-live-process reopen rebuilds volatile coordination. The opt-in `ownerless-stress` preset reruns the independent-table reader/writer stress case at 200 writer iterations and 400 reader polls, with deterministic independent-table stress SQL trace export covering the same per-table writer schedule and aggregate reader oracle for external harness input. It runs concurrent DDL workers with live DML readers/writers plus forced `.shm` rebuild and native exclusive reopen checks, and deterministic DDL stress SQL trace export covers the same create/alter/index/rename/truncate/drop plus DML schedule for external harness input, deterministic DDL lifecycle SQL trace export covers create/rename/truncate/force/drop/recreate final-state oracles for external harness input, runs same-name temporary-table churn across ownerless processes plus forced `.shm` rebuild and native exclusive reopen checks for the resulting permanent table, and deterministic temporary-table stress SQL trace export covers the same session-local temporary-table churn plus post-worker permanent-table oracle for external harness input, runs explicit multi-statement transaction/savepoint stress with deterministic aggregate checks, forced `.shm` rebuild checks, native exclusive reopen checks, and deterministic transaction stress SQL trace export for external harness input, runs shared-table checksum stress with mixed direct/prepared DML writers plus deterministic sum/version/weighted-sum oracle checks, forced `.shm` rebuild checks, native exclusive reopen checks, and deterministic checksum stress SQL trace export for external harness input, runs pseudo-random shared-table transaction stress with savepoint rollback, full rollback, live aggregate-reader bounds, bounded retry on 1205/1213, deterministic final oracles, forced `.shm` rebuild checks, and native exclusive reopen checks, and runs foreign-key graph stress with concurrent ownerless workers over `CASCADE`, `SET NULL`, and `RESTRICT` edges, bounded retry on 1205/1213, tracked transaction page LSN coverage, DML current-read refresh, deterministic aggregate/referential oracles, forced `.shm` rebuild checks, and native exclusive reopen checks, each with a 900-second timeout; every stress case also asserts final no-peer page-version WAL reclamation. The `ownerless-test-hooks` preset adds unsafe deterministic transaction-registration, page-version before-append and after-append publish/checkpoint, page-visible publish-before-checkpoint, page-visible-checkpoint, redo-reservation, redo-gap writer blocking, redo completed-write, redo latest-before-checkpoint, redo latest-after-checkpoint, native checkpoint reclamation crash/race with resumed-closer WAL non-growth after newer peer commits, primitive active-pin page-version boundary reclamation, consistent-snapshot pre-execution pin/race coverage, dictionary-DDL begin/before-finish/after-finish crash injection, unique-index replacement crash recovery, same-schema, cross-schema, and same-schema multi-pair `RENAME TABLE` crash-at-dictionary-before-finish file-move recovery, view replacement/alter, column-list, check-option, nested check-option, and security crash recovery, and negative-proof coverage |
| Ownerless DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills same-schema, cross-schema, and same-schema multi-pair swap `RENAME TABLE` writers after native file movement, standalone `CREATE INDEX` and `DROP INDEX` writers after native secondary-index metadata creation/removal, duplicate top-level `CREATE INDEX IF NOT EXISTS`, missing top-level `DROP INDEX IF EXISTS`, duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, missing `ALTER TABLE ... DROP INDEX IF EXISTS`, duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS`, and duplicate `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers after MariaDB success, a `CREATE OR REPLACE UNIQUE INDEX` writer after native replacement metadata, an `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY` writer after native primary-key replacement, duplicate `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` no-op writer after MariaDB success, `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` and `ALTER TABLE ... DROP FOREIGN KEY` writers after native foreign-key metadata creation/removal, `ALTER TABLE ... ADD CONSTRAINT ... CHECK`, `ALTER TABLE ... DROP CONSTRAINT` for CHECK constraints, plus `ALTER TABLE ... ADD COLUMN`, `ALTER TABLE ... DROP COLUMN`, `ALTER TABLE ... MODIFY COLUMN`, `ALTER TABLE ... RENAME COLUMN`, duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`, missing `ALTER TABLE ... DROP COLUMN IF EXISTS`, missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS`, missing `ALTER TABLE ... RENAME COLUMN IF EXISTS`, missing `ALTER TABLE ... CHANGE COLUMN IF EXISTS`, missing `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and missing `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` writers after native table-definition mutation or MariaDB no-op success, including missing rename/change/default no-ops over generated-column/CHECK expression metadata, simple `CREATE VIEW` and `DROP VIEW` writers after native view definition-file creation/removal, `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view definition rewrite, explicit column-list create/replace/alter writers after native alias metadata storage/rewrite, check-option create/replacement/alter writers after native check-option metadata storage/rewrite, nested check-option outer-replacement and inner-alter writers after native nested view definition rewrite, explicit definer create and invoker replacement view writers after native security metadata storage/rewrite, duplicate `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op writers after MariaDB success, an `ALTER TABLE ... FORCE, ALGORITHM=COPY` writer after native table-copy rebuild, an `ALTER TABLE ... CONVERT TO CHARACTER SET` writer after native charset-conversion metadata/storage update, `ALTER TABLE ... ROW_FORMAT=DYNAMIC`, `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`, `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`, and `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16` writers after native row-format rebuilds, an `ALTER TABLE ... COMMENT` writer after native table-comment metadata update, an `ALTER TABLE ... AUTO_INCREMENT` writer after native high-watermark persistence, an `ALTER COLUMN ... SET DEFAULT` writer after native default metadata update, a `TRUNCATE TABLE` writer after native truncate/recreate, a `DROP TABLE` writer after native file removal, a `DROP DATABASE` writer after native schema/table removal, an `ALTER DATABASE` writer after native `db.opt` rewrite, and duplicate `CREATE DATABASE IF NOT EXISTS` plus missing `DROP SCHEMA IF EXISTS` no-op writers after MariaDB success, but before ownerless dictionary finish; it verifies live-peer cleanup remains busy until no-live recovery, verifies recovered present/absent secondary-index metadata, forced-index reads for create, forced-index rejection for drop, recovered idempotent secondary-index no-op behavior with original key-part preservation, missing-index absence, errno 1061 for plain duplicate create, post-recovery writes, and forced-index reads from top-level and ALTER-table no-op branches, recovered unique-index idempotent no-op behavior with original unique-key preservation, attempted-key non-enforcement, errno 1061 for plain duplicate create or ALTER add, and duplicate-key enforcement, recovered replacement unique-index metadata and duplicate-key enforcement, recovered primary-key replacement metadata with duplicate-key enforcement on the replacement key and duplicate values allowed on the former key, recovered primary-key idempotent no-op behavior with original-key preservation and candidate-key non-uniqueness, recovered added foreign-key metadata with orphan-row rejection and valid child writes, recovered dropped foreign-key metadata absence with orphan-row writes and parent deletes allowed, recovered generated-column FK ADD metadata/enforcement and generated-column FK DROP metadata absence with orphan writes and parent deletes allowed, recovered CHECK metadata with errno 4025 enforcement, recovered dropped-CHECK metadata absence with formerly invalid writes allowed, recovered present/absent view metadata, `.frm` file state, view query behavior, replacement/altered view column metadata, explicit column-list alias metadata and ordinal positions, stale alias rejection, recovered check-option and nested check-option metadata, updatability metadata, errno 1369 invalid-DML enforcement, old exposed-column rejection, recovered security type and non-empty definer metadata, original view-definition preservation, missing-view absence, and base-table writes, recovered added-column metadata/default values, recovered idempotent column no-op behavior with original default preservation, missing-column absence, errno 1060 for plain duplicate add, errno 1091 for plain missing drop, missing modify/rename/change/default no-op preservation with errno 1054 for plain retries, expression-table missing rename/change/default preservation with generated-column values, real defaults, and CHECK enforcement unchanged, and post-recovery writes, recovered column-default metadata and post-recovery default-backed insert behavior, recovered charset/collation metadata and retained/post-recovery rows, recovered table-comment metadata and retained/post-recovery rows, absent dropped-column metadata, recovered modified-column width/default metadata and widened-value writes, recovered renamed-column metadata with old-name rejection and new-name writes, recovered schema default metadata with pre-alter table collation preservation and post-recovery default inheritance, recovered schema idempotent no-op behavior with original defaults, real schema/table preservation, missing-schema absence, and errno 1007 for plain duplicate create, recovered AUTO_INCREMENT high-watermark metadata with monotonic implicit ID allocation, recovered generated-column and CHECK expression behavior after a dependent column rename, recovered force-rebuilt InnoDB table/space/index metadata, copied payload bytes, recovered dynamic and compressed row-format metadata, retained row payloads, compressed 4 KiB, 8 KiB, and 16 KiB ZBLOB page evidence, and post-recovery writes, verifies ownerless reopen before and after forced `.shm` rebuild, and verifies native exclusive reopen of the recovered table, column, index, constraint, view, or schema state after rebuild |
| Ownerless table-copy DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills representative `CREATE TABLE ... LIKE` and `CREATE TABLE ... SELECT` writers after native destination table creation, including CTAS row population, but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered `.frm`/`.ibd` files, `INFORMATION_SCHEMA.TABLES` and column metadata, copied secondary-index metadata for `LIKE`, CTAS copied rows, post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless table-replacement DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills representative `CREATE OR REPLACE TABLE`, `CREATE OR REPLACE TABLE ... LIKE`, and `CREATE OR REPLACE TABLE ... AS SELECT` writers after native old-table replacement or replacement-copy completion but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered replacement `.frm`/`.ibd` files, old-column and old-index absence, new-column and new-index metadata, empty replacement rowset for ordinary and LIKE replacement, copied secondary-index metadata for LIKE, CTAS copied rows, post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless table-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE TABLE IF NOT EXISTS` and missing `DROP TABLE IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original table definition and keeps plain duplicate create returning errno 1050, missing drop preserves the real table while keeping the missing table metadata/files absent, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless schema-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE DATABASE IF NOT EXISTS` and missing `DROP SCHEMA IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original schema defaults and inherited column collation while plain duplicate create returns errno 1007, missing drop preserves the real schema/table while keeping the missing schema metadata/directory absent, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless index-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate top-level `CREATE INDEX IF NOT EXISTS` and missing top-level `DROP INDEX IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original key part and keeps plain duplicate create returning errno 1061, missing drop preserves the real index while keeping the missing index metadata absent, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless ALTER index-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and missing `ALTER TABLE ... DROP INDEX IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original key part and keeps plain duplicate ALTER-add returning errno 1061, missing drop preserves the real index while keeping the missing index metadata absent, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless unique-index idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create/add preserves the original unique key and keeps plain duplicate create or ALTER-add returning errno 1061, the attempted replacement key remains non-enforced, duplicate-key enforcement for the original unique key remains active, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless unique-index drop DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills a `DROP INDEX` writer for an active unique secondary index after native metadata removal but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, the unique-index metadata is absent, forced-index reads on the dropped name fail, the formerly duplicate key shape inserts successfully after recovery, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless primary-key DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills plain `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)` and composite direction `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)` writers after native clustered-key rebuild but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered primary-key metadata and key-part direction, old-key absence from `PRIMARY`, duplicate-key enforcement on the replacement key, old-key duplicate writes where applicable, forced-index reads, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless view-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original view definition and keeps plain duplicate create returning errno 1050, missing drop preserves the real view while keeping the missing view metadata/files absent, post-recovery base-table writes remain visible through the view, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless primary-key idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original `PRIMARY(id)` clustered key and keeps plain duplicate primary-key add returning errno 1068, the attempted candidate `code` key remains non-unique, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless column-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original column and default while plain duplicate add returns errno 1060, missing drop preserves the real column while keeping the missing column absent and plain missing drop returning errno 1091, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless column IF EXISTS DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS`, `ALTER TABLE ... RENAME COLUMN IF EXISTS`, `ALTER TABLE ... CHANGE COLUMN IF EXISTS`, `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, these paths preserve the original real column metadata/default, keep the missing and attempted renamed/changed columns absent, keep plain missing modify/rename/change/default retries returning errno 1054, allow post-recovery writes, and ownerless/native reopen plus forced `.shm` rebuild observe the same state; missing `RENAME COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`, and default-alter variants are also covered on generated-column/CHECK expression tables, proving the real column, stored and virtual generated expressions, real-column defaults, CHECK enforcement, and missing attempted names survive recovery; external randomized DDL oracle execution remains planned |
| Ownerless index metadata crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset also kills `CREATE OR REPLACE UNIQUE INDEX`, unique-secondary `DROP INDEX`, `ALTER TABLE ... RENAME INDEX`, and `ALTER TABLE ... ALTER INDEX ... IGNORED`/`NOT IGNORED` writers after native index metadata changes but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, old/new index-name metadata, replacement unique-key metadata/enforcement, dropped unique-index metadata absence and duplicate-key release, ignored/not-ignored metadata, final forced-index reads, later writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless trigger DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills simple `CREATE TRIGGER` and `DROP TRIGGER`, `CREATE OR REPLACE TRIGGER`, ordered `CREATE TRIGGER ... PRECEDES ...`, duplicate `CREATE TRIGGER IF NOT EXISTS`, missing `DROP TRIGGER IF EXISTS`, delayed missing-dependency `CREATE TRIGGER`, explicit `CREATE DEFINER=CURRENT_USER TRIGGER`, and stored-function-body `CREATE TRIGGER` writers after native `.TRG`/`.TRN` metadata creation/removal, rewrite, no-op preservation, delayed dependency acceptance, or definer metadata storage but before ownerless dictionary finish; it verifies live-peer cleanup remains busy until no-live recovery, recovered present/absent `INFORMATION_SCHEMA.TRIGGERS` metadata including non-empty definer metadata for the explicit-definer case, recovered trigger-file presence/absence, trigger firing after recovered create, replacement, idempotent no-op preservation, delayed dependency creation, definer recovery, stored-function trigger metadata recovery, ownerless stored-routine execution rejection before base-row mutation, and ordinary native stored-function trigger firing, recovered MariaDB 1146 failure when the missing dependency is absent, recovered `ACTION_ORDER`/firing order after ordered create, dropped-trigger non-firing after recovered drop, absent missing-trigger `.TRN` state after recovered missing drop, `SHOW CREATE TRIGGER` for recovered present triggers including explicit `DEFINER=` metadata and rejection for the dropped trigger, ownerless reopen before and after forced `.shm` rebuild, and native exclusive reopen after rebuild, while broader privilege/security and randomized trigger crash variants remain planned |
| Ownerless online DDL option matrix | 🟡&nbsp;Partial | Focused ownerless SQL coverage verifies already-open peer refresh, final ownerless/native reopen, and forced `.shm` rebuild for accepted ordinary secondary-index `ALGORITHM=NOCOPY, LOCK=SHARED`, `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`, `ALGORITHM=INPLACE, LOCK=NONE`, `ALGORITHM=INPLACE, LOCK=SHARED`, and `ALGORITHM=INPLACE, LOCK=EXCLUSIVE` add/drop combinations plus a unique secondary-index `ALGORITHM=INPLACE, LOCK=SHARED` add/drop pair with duplicate-key enforcement, extending the existing `NOCOPY`, `INPLACE`, `INSTANT`, and `COPY` option coverage while broader randomized DDL oracle execution remains planned |
| Ownerless READ UNCOMMITTED policy | 🟡&nbsp;Partial | Ownerless read/write opens reject `SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED`, session-scoped READ UNCOMMITTED changes, all `tx_isolation`/`transaction_isolation` assignments, and `SET STATEMENT` isolation overrides before entering unproven cross-process dirty-read semantics or untracked isolation-variable state; ordinary exclusive embedded opens still inherit MariaDB/InnoDB `READ UNCOMMITTED` behavior |
| Ownerless checkpoint hot-path publication | 🟡&nbsp;Partial | Hook-driven ownerless raw-latest and page-visible checkpoint writes now snapshot shared redo state while holding the checkpoint byte-range lock and write the monotonic latest/visible pair without rereading `.ckpt`; startup baseline seeding and no-live native checkpoint promotion still use the file-read merge path, durable sync ordering remains unchanged, crash-hook coverage remains required for redo latest and visible publication, and lazy/batched checkpoint writes remain planned until the `.ckpt` record has torn-write detection |
| Ownerless expanding-page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers distinct large-row updates across an expanding data-page set while a repeatable-read snapshot pin remains live, with retained page-version WAL during the pin, native checkpoint proof and WAL checkpoint after release, and ownerless/native reopen after forced `.shm` rebuild once the reader releases; no-live close-time reclamation also has deterministic coverage for a raw-latest/page-visible checkpoint gap during retained-WAL pressure; opt-in `ownerless_page_log_limit_bytes` write throttling covers direct/prepared writes including prepared `INSERT ... SELECT`, representative DML/DDL write classes, variant DML/upsert/index/rename/truncate spellings, DML modifier spellings, column ALTER variants, CHECK and FOREIGN KEY constraint DDL variants, storage/rebuild ALTER variants, AUTO_INCREMENT DDL high-watermark ALTER, generated-column DDL/index variants, generated-column FK DDL variants, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants at the first user-visible WAL pressure limit, `mylite_ownerless_pressure_status()` exposes the current active-pin/WAL throttle state, thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling is covered when no peer process is live; live-idle coverage proves native-support proof WAL remains retained while a peer is live and is reclaimed after the peer closes, while live writer and active-pin coverage retain user WAL, timer-driven checkpoint scheduling is covered after shared read-only snapshot release without another writer SQL statement and now proves an active prepared result cursor keeps retained WAL from being reclaimed until finalized, and deterministic active-reader pressure SQL trace export now provides external harness input with replacement-copy DDL and AUTO_INCREMENT high-watermark ALTER oracles plus bounded raw-client retry for MariaDB `1020`/`1205`/`1213` and SQLSTATE `40001` contention, plus Docker-backed MariaDB 11.8 scale-3 replay evidence, while full external MariaDB/RQG oracle stress remains planned |
| Ownerless pressure DDL variants | 🟡&nbsp;Partial | The active-reader pressure write-policy selector now also throttles column `ALTER TABLE ... MODIFY COLUMN`, `CHANGE COLUMN`, `DROP COLUMN`, `RENAME COLUMN`, `ALTER COLUMN ... SET DEFAULT`, and `ALTER COLUMN ... DROP DEFAULT`, CHECK constraint add/drop, FOREIGN KEY add/drop, charset conversion, `ALTER TABLE ... FORCE`, row-format rebuild, `ALTER TABLE ... AUTO_INCREMENT` high-watermark DDL, generated-column ALTER, generated-column secondary-index create/drop, stored generated-column child FK ADD, and stored generated-column referenced-FK DROP, plus `ALTER DATABASE`, duplicate `CREATE TABLE IF NOT EXISTS`, missing and real `DROP TABLE IF EXISTS`, `CREATE OR REPLACE TABLE ... LIKE`, `CREATE OR REPLACE TABLE ... AS SELECT`, `CREATE OR REPLACE VIEW`, `ALTER VIEW`, `CREATE OR REPLACE TRIGGER`, duplicate `CREATE TRIGGER IF NOT EXISTS`, and missing and real `DROP TRIGGER IF EXISTS` while retained WAL is at the configured ownerless pressure limit, verifies those blocked variants leave column metadata/defaults, CHECK and FK metadata, charset/collation, row-format metadata, AUTO_INCREMENT high-watermark state, generated-column, generated-index, and generated-column FK metadata, schema defaults, table state, replacement-copy target metadata, view projection, trigger bodies, and trigger presence unchanged, also verifies representative unsupported ownerless table-admin, `LOCK TABLES`, flush read-lock/export, host-file import, event/scheduler SQL including prepared event DDL/metadata, top-level sequence DDL/value SQL, `DISCARD TABLESPACE`, and rejected storage-option SQL keep explicit policy errors instead of pressure busy under the same retained-WAL limit, and verifies the same statement families succeed after reader release with final ownerless/native reopen before and after forced `.shm` rebuild |
| Ownerless BLOB page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers `ROW_FORMAT=DYNAMIC` off-page `LONGBLOB` payload updates that create native InnoDB BLOB page types, while a repeatable-read snapshot pin continues to read the original BLOB aggregates; coverage verifies retained page-version WAL during the pin, checkpoint after release, ownerless/native reopen before and after forced `.shm` rebuild for the final BLOB aggregates, and a bounded 12 KiB / 24 KiB / 48 KiB / 96 KiB / 192 KiB long-value size matrix, while exhaustive long-value limits, broader row-format, encryption, crash, and external oracle matrices remain planned |
| Ownerless compressed BLOB page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` off-page `LONGBLOB` payload updates created through prepared binary bindings that produce native InnoDB `ZBLOB`/`ZBLOB2` page types, while a repeatable-read snapshot pin continues to read the original compressed BLOB aggregates; coverage verifies retained page-version WAL during the pin, checkpoint after release, ownerless/native reopen before and after forced `.shm` rebuild for final aggregates, a bounded 12 KiB / 24 KiB / 48 KiB / 96 KiB / 192 KiB compressed long-value size matrix, and a bounded compressed `KEY_BLOCK_SIZE=1` / `2` / `4` / `8` / `16` matrix, while broader compressed DDL option combinations, encryption, crash, and external oracle matrices remain planned |
| Ownerless BLOB pressure trace export | 🟡&nbsp;Partial | `tools/ownerless-blob-pressure-trace` emits deterministic external-harness SQL for `ROW_FORMAT=DYNAMIC` and `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` `LONGBLOB` pressure, including a retry-aware repeatable-read snapshot reader with bounded MariaDB `1020`/`1205`/`1213` and SQLSTATE `40001` handling, a retry-aware concurrent update worker with bounded MariaDB `1205`/`1213` and SQLSTATE `40001` handling, manifest metadata, and final aggregate oracles; it is part of the deterministic ownerless SQL trace suite and Docker-backed external MariaDB trace replay, with recorded MariaDB 11.8 scale-3 replay evidence for the combined dynamic/compressed BLOB pressure trace and full scale-2 replay evidence for all 11 deterministic traces, while full external MariaDB/RQG long-running BLOB pressure stress remains planned |
| Ownerless CTAS DML trace export | 🟡&nbsp;Partial | `tools/ownerless-ctas-dml-trace` emits deterministic external-harness SQL for repeated `CREATE TABLE ... ENGINE=InnoDB AS SELECT` followed by post-create `UPDATE`, `DELETE`, and `INSERT` statements, a repeatable-read snapshot reader over a stable aggregate table, manifest metadata, and final table/column/aggregate oracles; it is part of the deterministic ownerless SQL trace suite, has dependency-free generator, trace-runner, and suite check-mode coverage, and passed focused plus full-suite Docker-backed MariaDB 11.8 scale-2 replay with `trace_count=11`, while randomized CTAS DML/RQG stress remains planned |
| Test-only directory-lock bypass | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset still has an environment-controlled `mylite.lock` bypass for negative-proof tests; a second process over the same directory must fail or hang within the bounded proof test, and the hook is unavailable in normal builds |
