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
| Ownerless cross-process SQL | `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`, registered as sixteen deterministic weighted CTest shards with flushed per-case progress diagnostics so long ownerless SQL coverage reports per-shard failures, estimated shard weights, timings, active case names, active case indexes, and `/proc` process-group/thread state on timeout; hidden per-case children run in their own process groups so timeout cleanup cannot leave orphaned descendants holding CTest output pipes open; a timed-out case can be rerun directly with `mylite_ownerless_cross_process_sql_test sql-case <index-or-name>` through the same wrapper, `sql-case-count` reports the current direct-case loop bound, and the old modulo `sql-shard <index> <count>` command remains available for comparison; earlier two-job and four-job modulo-shard scheduling produced load-sensitive ownerless DDL/dictionary/temporary-tablespace timeouts, while weighted-shard ownerless SQL measurement passed at two jobs locally with about half the wall time; a later smaller-shard slice split the registered weighted group from eight to sixteen shards for more granular timings without weakening the 300-second per-case watchdog; full-preset two-job scheduling still timed out when ownerless and non-ownerless tests interleaved, so CI runs non-ownerless embedded tests and ownerless SQL as separate visible production steps; the non-ownerless step runs serially with `--output-on-failure` after local reproduction showed independent MariaDB embedded CTest processes can interfere during startup/shutdown, while the ownerless SQL step now uses the registered sixteen weighted shards under `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 2 --output-on-failure` so CI reports per-shard timings instead of hiding the suite behind a monolithic direct-case loop; shard 8 (`test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`) and shard 11 (`test_ownerless_foreign_key_cross_schema_child_rename_refreshes_peer_dictionary`) are marked `RUN_SERIAL` after direct-case and isolated-shard reruns proved their observed CI timeouts were load-sensitive rather than standalone failures |
| Ownerless cross-process stress | `cmake --preset ownerless-stress && cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test && ctest --preset ownerless-stress`, covering independent-table, DDL/DML, temporary-table, explicit-transaction, checksum-oracle, pseudo-random shared-table transaction, foreign-key graph, stress child-failure cleanup, active-reader pressure, expanding-page pressure, BLOB page pressure, compressed BLOB page pressure stress cases, deterministic SQL trace exporters including DDL lifecycle with same-name recreate `SPACE` identity, cross-schema rename `SPACE` identity, dropped-schema absence, and replacement-copy DDL oracles, CTAS post-create DML, active-reader pressure including AUTO_INCREMENT high-watermark final oracle, compressed row-format DDL with the `KEY_BLOCK_SIZE=1`/`2`/`4`/`8`/`16` option cycle and InnoDB zip-page metadata checks, and BLOB pressure, full deterministic trace-suite validation, seeded random transaction, DDL stress, and FK graph trace validation, scaled check-mode validation for active-reader, compressed row-format DDL, and BLOB pressure traces, opt-in Docker-backed external MariaDB deterministic trace replay with focused `--trace` and bounded `--scale` profiles including refreshed full scale-2 deterministic replay evidence for the current 12 trace families, focused DDL lifecycle cross-schema/schema-drop replay evidence, focused seeded DDL stress replay evidence, combined random-transaction/DDL/FK seed-sweep replay evidence through seed `1151`, and the external trace-runner fake-client smoke test |
| Ownerless negative proof | `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof` |
| Platform probes | `ctest --preset embedded-dev -L compat.platform`; hook-only ownerless open rejection is covered by `ctest --preset ownerless-test-hooks -R libmylite.ownerless-platform-probe-failure` |
| Application queries | `ctest --preset embedded-dev -L compat.application-query` |
| Engine clauses | `ctest --preset embedded-dev -L compat.engine` |
| Server surfaces | `ctest --preset embedded-dev -L compat.server-surface` |
| Current SQL query surface | `ctest --preset embedded-dev -L compat.query` |

The MariaDB-reference group uses expected result vectors pinned to MariaDB 11.8
behavior. It does not require a daemon in the default test path.

The deterministic ownerless SQL trace suite currently contains 12 trace
families after adding compressed row-format DDL export; the DDL lifecycle trace
records per-round same-name recreated and cross-schema-renamed InnoDB `SPACE`
identity oracles plus dropped-schema absence oracles in its generated SQL, the
active-reader pressure trace includes an AUTO_INCREMENT
high-watermark final oracle, and the compressed row-format trace records
bounded `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE` `1`/`2`/`4`/`8`/`16`
rebuild cycles plus InnoDB `ZIP_PAGE_SIZE` metadata checks. The first full
12-family Docker-backed MariaDB 11.8 scale-2 replay exposed a CTAS reader
raw-client `ERROR 1020` under concurrent CTAS/DML replay, so the CTAS trace
reader now has a bounded retry contract. The rerun passed all 12 deterministic
traces at scale 2 with `trace_count=12`, `suite_run=ok`, and
`external_mariadb_trace_smoke=ok`, including the DDL lifecycle, CTAS,
active-reader, compressed row-format, and BLOB pressure traces.
Focused Docker-backed MariaDB 11.8 replay of the updated DDL lifecycle
cross-schema/schema-drop trace passed at scale 1 with `trace_count=1`,
`suite_run=ok`, and `external_mariadb_trace_smoke=ok`.
Docker-backed MariaDB 11.8 random transaction and DDL stress seed-suite replay
covers seeds `0`, `17`, `83`, and `211` at rounds `8`; the FK graph trace now
also has seeded trace-suite and optional Docker-smoke entry points using the
same default seed set, with bounded Docker-backed MariaDB 11.8 replay evidence
for seeds `0`, `17`, `83`, and `211` at rounds `2` after whole-seed retries
recovered transient raw MariaDB `1213` escapes for seeds `17` and `83`.
Dependency-free CTest
check-mode coverage validates the random transaction, DDL, and FK graph seeded
wrappers plus their external wrapper plans with the same seed set, plus the
combined seed-sweep wrapper over seeds `0` through `15` at rounds `3`, a
wider combined seed-sweep wrapper over seeds `0` through `63` at rounds `2`,
and a next combined seed-sweep wrapper over seeds `1120` through `1151` at rounds
`2`;
a focused seed-sweep replay through one disposable MariaDB 11.8 server covers
the random transaction and DDL seeded suites over seeds `0`
through `7` at rounds `4`, and a follow-up replay covers seeds `8` through
`15` at rounds `2`. A subsequent Docker-backed replay covers the random
transaction, DDL stress, and FK graph seeded suites over seeds `16` through
`31` at rounds `2`, with FK graph whole-seed retries recovering transient raw
MariaDB `1213` exits for seeds `17`, `21`, and `25`. A follow-up replay covers
the same three seeded suites over seeds `32` through `63` at rounds `2`, with
FK graph whole-seed retries recovering transient raw MariaDB `1213` exits for
seeds `34`, `35`, `36`, and `37`. A further replay covers the same three
seeded suites over seeds `64` through `95` at rounds `2`, with FK graph
whole-seed retries recovering transient raw MariaDB `1213` exits for seeds
`64`, `81`, and `85`. The next replay covers the same three seeded suites over
seeds `96` through `127` at rounds `2`, with FK graph whole-seed retries
recovering transient raw MariaDB `1213` exits for seeds `106`, `113`, and
`118`. Another replay covers seeds `128` through `159` at rounds `2`, with FK
graph whole-seed retries recovering transient raw MariaDB `1213` exits for
seeds `129` and `132`. A further replay covers seeds `160` through `191` at
rounds `2`, with FK graph whole-seed retries recovering transient raw MariaDB
`1213` exits for seeds `160`, `162`, `164`, `168`, `170`, and `176`. The next
replay covers seeds `192` through `223` at rounds `2`, with FK graph
whole-seed retries recovering transient raw MariaDB `1213` exits for seeds
`211`, `213`, `218`, and `219`. The next replay covers seeds `224` through
`255` at rounds `2`, with the combined wrapper recording an explicit FK graph
whole-seed retry budget and recovering transient raw MariaDB `1213` exits for
seeds `229`, `232`, `233`, `234`, `239`, `242`, and `252`. The next replay
covers seeds `256` through `287` at rounds `2` with the same explicit FK graph
retry budget, recovering transient raw MariaDB `1213` exits for seeds `260`,
`267`, `277`, and `280`. The next replay covers seeds `288` through `319` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seeds `288`, `294`, `298`, `304`, `308`, and
`310`. The next replay covers seeds `320` through `351` at rounds `2` with the
same explicit FK graph retry budget, recovering transient raw MariaDB `1213`
exits for seeds `344` and `348`. The next replay covers seeds `352` through
`383` at rounds `2` with the same explicit FK graph retry budget, recovering
transient raw MariaDB `1213` exits for seed `354` on attempt `4`, seeds `357`
and `365` on attempt `3`, and seeds `358`, `362`, `363`, `368`, `369`, `371`,
`375`, and `379` on attempt `2`. The next replay covers seeds `384` through
`415` at rounds `2` with the same explicit FK graph retry budget, recovering
transient raw MariaDB `1213` exits for seed `403` on attempt `3`, seed `405` on
attempt `2`, seed `406` on attempt `5`, seed `408` on attempt `3`, and seed
`413` on attempt `2`. The next replay covers seeds `416` through `447` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seed `417` on attempt `3`, seed `418` on attempt
`2`, seed `419` on attempt `2`, seed `420` on attempt `5`, seed `423` on
attempt `3`, seed `425` on attempt `2`, seed `434` on attempt `2`, and seed
`436` on attempt `2`. The next replay covers seeds `448` through `479` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seed `449` on attempt `3`, seed `456` on attempt
`2`, seeds `463`, `466`, `467`, `471`, and `475` on attempt `3`, and seed
`477` on attempt `2`. The next replay covers seeds `480` through `511` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seed `481` on attempt `2`, seed `483` on attempt
`3`, seed `486` on attempt `2`, seed `490` on attempt `5`, seed `492` on
attempt `2`, seed `493` on attempt `4`, seed `500` on attempt `4`, seed `503`
on attempt `2`, seed `505` on attempt `3`, seeds `507`, `509`, and `510` on
attempt `2`, and seed `511` on attempt `3`.
The next replay covers seeds `512` through `543` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `519` on attempt `3`, and seeds `522`, `523`, `525`, `527`, `528`,
`536`, and `542` on attempt `2`.
The next replay covers seeds `544` through `575` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `545` on attempt `5`, seed `548` on attempt `4`, seeds `549` and
`557` on attempt `3`, and seeds `546`, `560`, `562`, `564`, and `575` on
attempt `2`. The next replay covers seeds `576` through `607` at rounds `2`
with the same explicit FK graph retry budget, recovering transient raw MariaDB
`1213` exits for seeds `581`, `586`, and `605` on attempt `4`, seeds `576`,
`584`, `595`, `599`, and `604` on attempt `3`, and seeds `578`, `579`, `582`,
`587`, `589`, `590`, and `606` on attempt `2`. The next replay covers seeds
`608` through `639` at rounds `2` with the same explicit FK graph retry
budget, recovering transient raw MariaDB `1213` exits for seeds `609`, `613`,
and `621` on attempt `3`, and seeds `608`, `616`, `618`, `626`, `633`, and
`634` on attempt `2`. The next replay covers seeds `640` through `671` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seed `657` on attempt `4`, seed `654` on attempt
`3`, and seeds `641`, `644`, `647`, `649`, `652`, `655`, `667`, `669`, and
`670` on attempt `2`. The next replay covers seeds `672` through `703` at
rounds `2` with the same explicit FK graph retry budget, recovering transient
raw MariaDB `1213` exits for seed `701` on attempt `6`, seed `689` on attempt
`5`, seed `678` on attempt `4`, seeds `693`, `698`, `699`, `700`, and `702`
on attempt `3`, and seeds `672`, `692`, `694`, and `703` on attempt `2`.
The next replay covers seeds `704` through `735` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `722` on attempt `5`, seeds `708`, `725`, and `726` on attempt `3`,
and seeds `704`, `706`, `713`, `715`, `718`, `724`, `728`, and `732` on
attempt `2`.
The next replay covers seeds `736` through `767` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seeds `737`, `738`, `755`, and `764` on attempt `4`, seeds `742`, `745`,
and `747` on attempt `3`, and seeds `736`, `744`, `746`, `750`, `757`, and
`765` on attempt `2`.
The next replay covers seeds `768` through `799` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `778` on attempt `4`, seeds `774`, `780`, and `788` on attempt `3`,
and seeds `769`, `777`, `785`, `787`, and `794` on attempt `2`.
The next replay covers seeds `800` through `831` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seeds `801` and `803` on attempt `4`, seeds `811` and `829` on attempt
`3`, and seeds `802`, `806`, `823`, `826`, and `830` on attempt `2`.
The next replay covers seeds `832` through `863` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `849` on attempt `4`, seeds `834` and `852` on attempt `3`, and seeds
`833`, `837`, `845`, `846`, and `851` on attempt `2`.
The next replay covers seeds `864` through `895` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `887` on attempt `5`, seed `872` on attempt `3`, and seeds `865`,
`870`, `878`, `879`, `880`, `886`, and `891` on attempt `2`.
The next replay covers seeds `896` through `927` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `905` on attempt `5`, seeds `904`, `910`, and `918` on attempt `3`,
and seeds `899` and `908` on attempt `2`.
The next replay covers seeds `928` through `959` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `957` on attempt `4`, seed `939` on attempt `3`, and seeds `932`,
`934`, `946`, and `956` on attempt `2`.
The next replay covers seeds `960` through `991` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `968` on attempt `5`, seeds `963`, `969`, and `985` on attempt `4`,
seeds `965`, `972`, `976`, `979`, and `984` on attempt `3`, and seeds `975`,
`978`, `981`, and `982` on attempt `2`.
The next replay covers seeds `992` through `1023` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seeds `992`, `993`, `994`, `997`, `999`, `1004`, `1011`, `1015`, `1020`,
and `1022` on attempt `2`.
The next replay covers seeds `1024` through `1055` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `1052` on attempt `7`, seeds `1026` and `1054` on attempt `3`, and
seeds `1029`, `1031`, `1042`, `1043`, `1053`, and `1055` on attempt `2`.
The next replay covers seeds `1056` through `1087` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seeds `1067` and `1074` on attempt `3`, and seeds `1060`, `1063`, `1068`,
`1070`, `1078`, and `1080` on attempt `2`.
The next replay covers seeds `1088` through `1119` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seed `1104` on attempt `6`, seeds `1096` and `1097` on attempt `5`, seed
`1114` on attempt `4`, seeds `1095` and `1103` on attempt `3`, and seeds
`1088`, `1094`, `1105`, `1108`, `1111`, and `1112` on attempt `2`.
The next replay covers seeds `1120` through `1151` at rounds `2` with the same
explicit FK graph retry budget, recovering transient raw MariaDB `1213` exits
for seeds `1120`, `1121`, and `1130` on attempt `3`, and seeds `1123`,
`1128`, `1133`, `1136`, `1150`, and `1151` on attempt `2`.
Longer randomized external MariaDB/RQG stress remains planned.

Ownerless stale-reader file-lifecycle replay now includes same-schema and
cross-schema same-statement multi-table `DROP TABLE` coverage for multiple
removed file-per-table tablespaces, rename-away plus new original-name
`CREATE TABLE` coverage that preserves both final file-per-table spaces, and
same-name `CREATE OR REPLACE TABLE` replacement coverage, including `... LIKE`
copied-shape replacement and the `... AS SELECT` populated replacement variant,
plus hook-build coverage for a representative `CREATE OR REPLACE TABLE` crash
after the old target is removed and before replacement creation begins,
in addition to the single-table drop, stale-reader retained-WAL killed-drop,
and multi-table schema-drop evidence tracked below. This is still bounded
replay evidence, not a claim that the
broader durable DDL file-lifecycle protocol is complete.
Primitive native tablespace replay also now proves duplicate page-0 FSP-header
tablespace candidates fail closed: strict replay errors, product skip mode
leaves both ambiguous files unchanged, and native boundary reads return
`NOT_FOUND` rather than choosing one candidate.
Ownerless checkpoint LSN publication now appends two checksummed generation
records to `mylite-concurrency.ckpt`; runtime reads prefer the highest valid
record, fall back to the legacy pair only before any record has been written,
and focused recovery coverage corrupts the latest record while preserving the
previous one.
The native file-operation checkpoint-needed marker also now uses two
checksummed generation records after the LSN records; runtime marker reads
prefer the highest valid record, fall back to the legacy marker only before any
marker record has been written, and treat corrupt marker-record-only evidence
as checkpoint-needed so a torn clear cannot suppress required native drain.
Ownerless dictionary DDL now persists that marker after native `FILE_*` redo
evidence and before the dictionary-finish hook window, with focused hook
coverage for `RENAME TABLE` `FILE_RENAME`, `CREATE TABLE ... LIKE`
`FILE_CREATE`, CTAS populated `FILE_CREATE`, `TRUNCATE TABLE` native
truncate/recreate, `DROP TABLE` `FILE_DELETE`, and replacement-copy
`CREATE OR REPLACE TABLE ... LIKE`/`CREATE OR REPLACE TABLE ... AS SELECT`
boundaries plus representative `ALTER TABLE ... FORCE` and
`ALTER TABLE ... ROW_FORMAT=DYNAMIC` rebuild boundaries, so a killed writer
cannot lose the durable checkpoint-needed boundary before no-live recovery for
those classes.
Focused hook coverage also now forces a native checkpoint, clears the
ownerless file-op redo flag, updates a file-per-table InnoDB table, and
observes the flag set again, proving ordinary post-checkpoint DML reaches
MariaDB's `FILE_MODIFY` redo path. That is observation evidence, not a broad
durable-marker claim for every DML-origin `FILE_MODIFY` case.
Ownerless successful autocommit non-DDL write cleanup now consumes the same
native file-op redo flag and persists the existing checkpoint-needed marker
when post-checkpoint DML emits file-operation redo. Focused SQL coverage forces
a checkpoint, runs an ownerless autocommit `UPDATE` on a file-per-table InnoDB
table, observes the marker before close, drains it on final no-live close,
forces `.shm` rebuild, and verifies ownerless and ordinary native reopen
preserve the updated row. This is bounded durable marker coverage for
checkpointed autocommit DML, not a claim that every possible DML-origin
`FILE_MODIFY` shape or explicit-transaction DML path has been exhaustively
classified.
Ownerless AUTO_INCREMENT publishes now also mark a shared registry
native-checkpoint pending bit when they raise a table high watermark. The
final no-live ownerless close path drains that bit through the existing native
checkpoint/reclaim path before clearing it, preserving duplicate-key-consumed
AUTO_INCREMENT gaps across forced `.shm` rebuild without adding per-insert
durable checkpoint writes.
Already-open ownerless peers now also recover a stale InnoDB dictionary-cache
miss for a peer-created file-per-table table after trigger DDL: ownerless text
and prepared plain reads that hit MariaDB errno `1932` refresh native pages,
evict the SQL and InnoDB dictionary caches, clear MyLite's ownerless
foreign-key cache, and retry once. The focused trigger DDL refresh case
verifies the peer reads the trigger-maintained audit table through a prepared
`SELECT` before any text read warms the cache, then observes its native
tablespace registration. Mutating statements, DDL, locking reads, and
explicit-transaction statements do not use this retry path.
Already-open ownerless peers that observe a dictionary generation change stay
in conservative native-read mode for table reads, but that native path now
forces a visible-boundary buffer-pool refresh without consulting page-version
WAL. This preserves the compressed/rebuilt table guard while allowing peer
committed foreign-key cascades and deletes to replace locally cached pages on
the same handle.

Ownerless performance diagnostics now run through production build presets for
CI-visible timings, and CI separates the default stats-off embedded throughput
probe, a stats-off large-row bulk probe, a reduced stats-enabled ownerless
attribution probe, and a reduced append-only ownerless attribution probe that
keeps page-publish stats disabled so the production history-proof pair hook is
measured directly. The startup probe now also reports a second fresh ownerless
directory on the same filesystem device after the first ownerless directory has
proved the platform, so CI can distinguish the child-process platform probe
from cold runtime/InnoDB startup. The large-row bulk probe uses
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` so CI reports the ownerless
default-checked bulk-insert row-list shape separately from the default four-row
small-bulk shape; it now uses `MYLITE_PERF_INSERT_ITERATIONS=5000`, producing
50 such statements instead of a timer-noisy five-statement sample. The
embedded probe now also splits ordinary and ownerless bulk timing into the
first row-list statement and the remaining row-list statements, exposing the
current empty-table default-checked bulk path separately from later
non-empty-table insert execution. Stats-enabled bulk attribution now mirrors
that split for the existing deep InnoDB comparison rows, emitting `first_` and
`remaining_` commit, row-insert, clustered B-tree, undo-report, and
default-checked bulk-start summaries without changing aggregate row names or
SQL behavior. The ownerless bulk page-write phase split now snapshots the
first row-list statement for existing page-publish, database hook, page-write,
page-log append, and commit-visibility counters, then emits `first_` and
`remaining_` phase rows so aggregate bulk output no longer hides the later
non-empty-table commit-log/no-dirty-loop cost. CI also separates the
WordPress PHPUnit source, build,
dependency, database-prep, performance-probe, and test-only phases so PHPUnit
wall timings are not hidden inside build work.
Embedded performance CI also writes a compact rollup table to
`build/embedded-performance-reports/summary.md` and appends it to the GitHub
step summary, surfacing warm open/close cost, point-select ratios, autocommit
insert ratio, ownerless bulk row throughput, ownerless bulk `mysql_query()`
time, and remaining non-empty-table undo-report MTR cost without downloading
the raw logs.
Those phases append compact timing rows into
`build/wordpress-phpunit-reports/timing-summary.md`, and CI publishes that
Markdown table to the GitHub step summary so setup, build, probe, and each
PHPUnit shard can be compared without scraping separate step logs. The split
runtime artifact handoff also records pack, upload, per-shard download, and
per-shard extract seconds plus runtime/database-baseline tarball byte sizes, so
branch/main timing comparisons can distinguish test execution from artifact
transfer overhead. The runtime artifact is now staged from a slim shard
payload instead of the full MariaDB/MyLite build trees: it keeps the
production CMake caches and manifest-hashed runtime files, prunes WordPress
Git object history from the shard copy, validates the manifest inside the
staged root, and reports `wordpress_artifact_pack_runtime_root_bytes` beside
the compressed tarball sizes. This changes CI artifact transport only; SQL,
mysqli, native storage, recovery, and ownerless behavior are unchanged. CI now
also excludes non-PHPUnit WordPress test trees and root Composer development
dependencies from the staged shard snapshot while retaining `src`,
`tests/phpunit`, the writable REST fixture directory under
`tests/qunit/fixtures`, root metadata, Composer autoload metadata, and Yoast
PHPUnit polyfills. CI now also uploads that timing summary as the
`wordpress-phpunit-timing-summary` artifact, and the embedded performance probes
persist their production output as the
`embedded-performance-reports` artifact, so branch/main timing comparisons can
download the same evidence after a run instead of relying only on log scraping.
The embedded ownerless SQL CI step uses the registered sixteen weighted CTest
shards through the production `php-embedded-prod` preset with `--parallel 2`,
and records the shard selector and parallelism in the GitHub step summary.
CTest still prints per-shard elapsed time and the existing child watchdog emits
active case identity plus process diagnostics when a case hangs.
The embedded performance and attribution probes run before embedded correctness
tests, so production throughput, large-row bulk, and attribution numbers remain
visible even when a later ownerless SQL case fails.
Stats-enabled prepared insert attribution now adds ordinary, ownerless, and
ownerless-minus-ordinary client timing summaries for `prepare`, transaction
begin, bind, `step`, reset, commit, finalize, measured loop time, and measured
residual time. This is diagnostic output only; stats-off throughput probes
remain the comparison signal for branch/main performance.
Ordinary embedded shutdown attribution now splits release teardown through
`mysql_thread_end()`, `mysql_server_end()`, `end_embedded_server()`,
`clean_up()`, and `plugin_shutdown()`. A reduced production sample narrowed the
dominant process-isolated lifecycle cost to storage-engine plugin
deinitialization during `reap_plugins()`: `plugin_shutdown_total_ms_avg` was
`223.868`, `plugin_shutdown_reap_deinitialize_ms_avg` was `223.822`, and 9
storage-engine deinit calls consumed `223.800 ms`, while 31 information-schema
deinit calls consumed `0.008 ms`. Follow-up storage-engine attribution
identified InnoDB handlerton panic shutdown as the dominant engine work:
`storage_engine_finalize_total_ms` was `216.159`, `storage_engine_finalize_panic_ms`
was `216.142`, and the single InnoDB finalizer consumed `215.685 ms`; Aria
consumed `0.458 ms` and other fixed engine buckets were below `0.006 ms`.
InnoDB shutdown attribution then narrowed the dominant cost to
`logs_empty_and_mark_files_at_shutdown()` waiting: `innodb_shutdown_total_ms`
was `248.695`, `innodb_shutdown_logs_empty_ms` was `232.035`, and the fixed
shutdown-loop sleep consumed `200.309 ms`, while checkpoint work consumed
`0.011 ms`. Embedded builds now skip the first log-empty sleep before running
the existing quiet-state checks; a reduced production sample after that change
reported `innodb_shutdown_total_ms=125.473`,
`innodb_shutdown_logs_empty_ms=101.397`, and
`innodb_logs_empty_sleep_ms=100.470`, with checkpoint work still `0.017 ms`.
A follow-up embedded-only bounded immediate-retry budget removes the remaining
fixed sleep for the ordinary warm sample: `innodb_shutdown_total_ms=16.315`,
`innodb_shutdown_logs_empty_ms=1.569`, and
`innodb_logs_empty_sleep_ms=0.000`, with checkpoint work still `0.012 ms`.
Repeated open/close attribution then showed the remaining fixed sleeps were
background-thread waits, not active transactions or checkpoint movement: a
ten-iteration reduced production sample reported 9 sleeps after background
retries and `902.541 ms` total sleep time. Embedded background-thread retry
sleep now uses a `1 ms` poll after the immediate retry budget is exhausted; a
ten-iteration sample after that change reported
`innodb_logs_empty_sleep_ms=11.280`, `innodb_logs_empty_total_ms=54.854`, and
no active-transaction or checkpoint retries.
The probe also reports direct multi-row `INSERT ... VALUES` row-list timing
with `mylite_perf_bulk_insert_rows_per_statement`,
ordinary/ownerless bulk row and statement throughput, ownerless/ordinary bulk
ratios, and stats-enabled per-row/per-statement ownerless attribution for page
versions, page-log appends, native-support publication, and commit-visibility
choices. This separates the single-row prepared autocommit cost from the
multi-row fast-path SQL shape that ownerless concurrency now admits.
Ownerless page-log append batching now covers streaming-proven visible-fast
`INSERT ... VALUES` statements with one through 32768 row constructors, matching
the default production bulk probe shape plus bounded eight-row, sixteen-row,
thirty-two-row, sixty-four-row, 128-row, 256-row, 512-row, 1024-row, and
2048-row, plus 4096-row, 8192-row, 16384-row, and 32768-row bulk diagnostics
while keeping row lists above 32768 and broader DML on the previous
append-session policy.
The row-list proof now scans the original SQL text instead of relying on the
fixed policy token snapshot, so large statements are not admitted by accidental
token-window undercounting. Page-version record volume, history-proof
native-support publication, WAL sync ordering, and page-visible LSN publication
are unchanged; the optimization only reduces repeated page-log append-session
begin/end work within the capped statement. The first stats-enabled production
probe for the four-row cap kept the default four-row bulk shape at `6.100`
append calls and `6.000` page-version records per statement, while
append-session begin/end calls dropped to `50` for `50` bulk statements from
the previous `250`/`250` sample. The eight-row follow-up preserves the same
one-session proof for a
larger bounded row list and re-enables deferred latest-checkpoint coalescing for
that shape: a stats-enabled production probe for `80` rows with eight rows per
statement reported `10` append-session begin/end calls for `10` statements,
`10.500` append calls and `10.000` page versions per statement, and `16.000`
deferred latest-checkpoint coalesces per statement. The sixteen-row follow-up
uses the same bounded proof for the next measured bulk shape: the pre-slice
production probe reported visible-fast commit at `1.000` per statement but
`171` append-session begin/end calls for `10` statements and zero coalesces;
the post-slice probe kept `185` page-log appends but reduced append-session
begin/end calls to `10`, enabled `320` deferred latest-checkpoint coalesces
(`32.000` per statement), and moved ownerless 16-row bulk throughput from
`7223.47` to `10655.43` rows/s on the same local shape. The thirty-two-row
baseline then showed visible-fast commit at `1.000` per statement but
`331` append-session begin/end calls for `10` statements and zero coalesces;
the post-slice probe kept `345` page-log appends but reduced append-session
begin/end calls to `10`, enabled `640` deferred latest-checkpoint coalesces
(`64.000` per statement), and moved ownerless 32-row bulk throughput from
`8345.99` to `9307.03` rows/s on the same local shape. The sixty-four-row
baseline stayed visible-fast at `1.000` per statement but showed `651`
append-session begin/end calls for `10` statements and zero coalesces; the
post-slice probe kept `669` page-log appends but reduced append-session
begin/end calls to `10`, enabled `1280` deferred latest-checkpoint coalesces
(`128.000` per statement), and moved ownerless 64-row bulk throughput from
`9856.67` to `11755.40` rows/s on the same local shape. The streaming row-count
follow-up preserves the already-observed 128-row and 256-row fast paths under a
real full-statement proof: pre-slice probes reported `10` append-session
begin/end calls, visible-fast commit at `1.000` per statement, and deferred
latest-checkpoint coalescing at `256.000` and `512.000` per statement
respectively, and that slice's focused SQL coverage proved the 512-row positive
boundary plus a 513-row conservative boundary with no deferred
latest-checkpoint coalescing. Post-slice production probes kept `10`
append-session begin/end
calls for `10` statements, visible-fast commit at `1.000` per statement, and
deferred latest-checkpoint coalescing at `256.000` and `512.000` per statement
for the 128-row and 256-row shapes respectively. Bounded visible-fast
append-batched statements in a single-owner epoch now also use the existing
transaction-deferred page publication proof for user data/index pages,
publishing only the final captured statement images at commit while preserving
native history WAL proof; peer-present ownerless statements keep immediate
page publication while retaining the append-session batching guard. On the
256-row production shape this moved page-log append calls from `2609` to `68`,
page-write publish total from `60.789 ms` to `2.938 ms`, commit-log publish
attribution from `61.565 ms` to `6.767 ms`, and ownerless bulk throughput from
`12178.41` to `20008.38` rows/s, while keeping visible-fast commit at `1.000`
per statement and conservative flush at `0.000`. Before the 512-row follow-up,
the 257-row guard probe remained outside that path with `2581` append-session
begin/end calls, `2619` page-log appends, and `2582` snapshot-boundary page
publications for `10` statements.
The 512-row cap follow-up moves the next bounded pure row-list shape onto the
same path: the reduced pre-slice probe showed `5131` append-session begin/end
calls, `5193` page-log appends, `5144` snapshot-boundary page publications,
zero deferred latest-checkpoint coalesces, and `11181.55` ownerless rows/s for
`10` statements; after the cap increase the same shape reported `10`
append-session begin/end calls, `90` page-log appends, zero snapshot-boundary
page publications, `1024.000` deferred latest-checkpoint coalesces per
statement, and `25810.38` ownerless rows/s while keeping visible-fast commit at
`1.000` per statement and conservative flush at `0.000`.
The 1024-row follow-up keeps the same parser-proven policy and moves the next
bounded row-list edge onto the append-batched path. Focused SQL coverage now
proves a 1024-row single-owner statement uses one append session and
transaction-deferred page publication without snapshot-boundary publication,
while the adjacent 1025-row statement stays outside append batching and
deferred latest-checkpoint coalescing. A reduced 10240-row production probe
with 1024 rows per statement reported `10` append-session begin/end calls for
`10` statements, `0` snapshot-boundary publications, `2.000` page versions and
native-support proof pages per statement, `1.000` visible-fast commits per
statement, and `2049.000` deferred latest-checkpoint coalesces per statement.
The 2048-row follow-up keeps the same parser-proven policy and moves the next
bounded row-list edge onto the append-batched path. Focused SQL coverage now
proves a 2048-row single-owner statement uses one append session and
transaction-deferred page publication without snapshot-boundary publication,
while the adjacent 2049-row statement stays outside append batching and
deferred latest-checkpoint coalescing. A reduced 20480-row production probe
with 2048 rows per statement reported `10` append-session begin/end calls for
`10` statements, `0` snapshot-boundary publications, `2.000` page versions and
native-support proof pages per statement, `1.000` visible-fast commits per
statement, and `115.200` deferred latest-checkpoint coalesces per statement.
The same sample reported ownerless/ordinary bulk rows ratio `0.3477`, with the
first statement at `1.3253` and later statements at `0.3195`; this keeps the
next write-performance target on remaining native row-insert/undo-report
attribution, not append-session churn. Larger row-list admission remained
planned separately at that boundary rather than inferred from the 2048-row
proof.
The 4096-row follow-up keeps the same parser-proven policy for the next bounded
row-list edge. Focused SQL coverage now proves a 4096-row single-owner
statement uses one append session and transaction-deferred page publication,
while the adjacent 4097-row statement stays outside append batching and
deferred latest-checkpoint coalescing. A reduced 8192-row production probe with
4096 rows per statement reported append-session begin/end calls dropping from
`8195`/`8195` before the cap increase to `2`/`2`, page-version records dropping
from `4136.000` to `2.000` per statement, page-log append calls dropping from
`4138.500` to `17.500` per statement, native-support proof pages dropping from
`21.000` to `2.000` per statement, visible-fast commits staying at `1.000` per
statement, deferred latest-checkpoint coalesces moving from `0.000` to
`128.000` per statement, ownerless `mysql_query()` moving from `424.537` to
`66.720 ms` per statement, and ownerless bulk throughput moving from `9587.08`
to `58904.02` rows/s on the reduced sample. At that boundary, larger row lists
remained planned rather than inferred from the bounded proof.
The 8192-row follow-up applies the same parser-proven policy to the next row-
list edge. Focused SQL coverage now proves an 8192-row single-owner statement
uses one append session and transaction-deferred page publication, while the
adjacent 8193-row statement stays outside append batching and deferred
latest-checkpoint coalescing. A reduced 16384-row production probe with 8192
rows per statement reported append-session begin/end calls dropping from
`16387`/`16387` before the cap increase to `2`/`2`, snapshot-boundary page
publications dropping from `16458` to `0`, page-version records dropping from
`8268.000` to `2.000` per statement, page-log append calls dropping from
`8270.500` to `27.500` per statement, native-support proof pages dropping from
`39.000` to `2.000` per statement, visible-fast commits staying at `1.000` per
statement, deferred latest-checkpoint coalesces moving from `0.000` to
`256.500` per statement, ownerless `mysql_query()` moving from `732.644` to
`130.506 ms` per statement, and ownerless bulk throughput moving from
`11128.17` to `61240.24` rows/s on the reduced sample. The later non-empty-
table statement still shows remaining native row-level undo/MTR cost, so row
lists above that edge and native undo work remained planned at that boundary.
The 16384-row follow-up applies the same parser-proven policy to the next row-
list edge. Focused SQL coverage now proves a 16384-row single-owner statement
uses one append session and transaction-deferred page publication, while the
adjacent 16385-row statement stays outside append batching and deferred
latest-checkpoint coalescing. A reduced 32768-row production probe with 16384
rows per statement reported `2` append-session begin/end calls, zero
snapshot-boundary publications, `2.000` page versions per statement, `45.000`
page-log appends per statement, `2.000` native-support published pages per
statement, visible-fast commits at `1.000` per statement, deferred
latest-checkpoint coalesces at `513.000` per statement, ownerless
`mysql_query()` at `220.533 ms` per statement, and ownerless bulk throughput at
`72415.68` rows/s with an ownerless/ordinary ratio of `0.6971`. The later
non-empty-table statement still shows native row-level undo/MTR cost, including
`301.249 ms` row-insert time and `74.959 ms` undo-report MTR commit time per
statement, so row lists above that edge and native undo work remained planned
at that boundary.
The 32768-row follow-up applies the same parser-proven policy to the next row-
list edge. Focused SQL coverage now proves a 32768-row single-owner statement
uses one append session and transaction-deferred page publication, while the
adjacent 32769-row statement stays outside append batching and deferred
latest-checkpoint coalescing. A reduced 65536-row production probe with 32768
rows per statement reported `2` append-session begin/end calls, zero
snapshot-boundary publications, `2.000` page versions per statement,
`1598.500` page-log appends per statement, `2.000` native-support published
pages per statement, visible-fast commits at `1.000` per statement, deferred
latest-checkpoint coalesces at `685.000` per statement, ownerless
`mysql_query()` at `422.337 ms` per statement, and ownerless bulk throughput at
`60797.81` rows/s with an ownerless/ordinary ratio of `0.9550`. The later
non-empty-table statement still shows native row-level undo/MTR cost, including
`115.835 ms` page-write commit-log time, `28.976 ms` redo-leave time, and
`153.153 ms` undo-report MTR commit time per statement, so row lists above
32768 and native undo work remain planned.
Ownerless transaction page tracking now adds a lazy exact membership cache over
the existing modified/dirty page vectors so repeated MTR and lock-hook page
ownership checks do not linearly scan large per-transaction page lists. The
vectors remain authoritative, caches are cleared with the vectors and rebuilt
after transaction gate erasure, and the change does not broaden the
default-checked bulk path for non-empty tables. The reduced 2048-row production
probe preserved `2048.000` remaining undo-report calls and `0.000` remaining
default-checked bulk starts per statement while moving later-statement
ownerless/ordinary rows ratio from `0.3195` to `0.3347`, remaining row-insert
time from `55.134 ms` to `51.364 ms` per statement, and remaining
undo-report MTR commit time from `16.498 ms` to `14.724 ms` per statement.
This remains a large row-list optimization; the dominant single-row
history-proof/native-support publication volume is still a separate target.
The history-proof publication harness then tightens the fast-path and unsafe
fallback selectors without changing production behavior: the controlled
single-owner insert shape now proves rollback-segment proof publication matches
published native-support `FIL_PAGE_TYPE_SYS`, undo proof publication matches
published native-support `FIL_PAGE_UNDO_LOG`, published native-support records
skip live page-index publication, and forced native-support publish failure
takes positive native history flush pages with zero accepted proof samples.
This keeps the current two-page proof contract explicit before any
proof-replacement work. A reduced stats-enabled production probe over 100
ownerless autocommit inserts reported `100` rollback-segment proof pages,
`100` undo proof pages, matching `100` published native-support
`FIL_PAGE_TYPE_SYS` and `100` published `FIL_PAGE_UNDO_LOG` pages, zero native
history flush pages on the fast path, and a page-index native-support skip
summary of `2.030` per insert.
Ownerless native-support history-proof WAL records now use explicit
proof-only metadata records instead of page-image records. The rollback-segment
and undo proof counts remain part of the same two-page proof contract, but the
records carry zero payload bytes, are rejected by page-image reads, are skipped
by page-version replay and checkpoint retained-record callbacks, and still
remain subject to the existing WAL retention and checkpoint rules. A reduced
stats-enabled production probe over 100 ownerless autocommit inserts preserved
`1.000` rollback-segment and `1.000` undo proof records per insert while
reporting `0.000 ms` in proof-path MTR scratch allocation, page copy, and page
checksum work, and `503.380` page-log payload bytes per insert. The matching
stats-off 500-row sample reported ownerless autocommit at `1950.43 ops/s`
versus `3560.43` ordinary ops/s, ownerless explicit transactions at
`2692.65 ops/s` versus `4171.71` ordinary ops/s, and ownerless four-row bulk
rows at `5328.72 rows/s`.
Ownerless production/stats-off history-proof publication can now append those
two existing proof-only records through one narrow paired native hook while the
history MTR still owns both page latches. The durable WAL format is unchanged:
the path still writes one rollback-segment proof-only record and one
undo-header proof-only record, marks both proof flags only after both appends
succeed, and falls back to the native exact history flush if the pair hook is
unavailable or fails. Detailed page-publish attribution and unsafe ownerless
fault builds deliberately stay on the previous per-page hook path so existing
page-publish counters and crash-window tests remain exact. Focused SQL
coverage proves pair calls succeed with zero exact history flush pages while
the detailed-counter proof path remains covered separately; short production
stats-off throughput samples remained noisy, so this is tracked as a bounded
hook/append overhead reduction rather than a broad throughput-completion
claim.
Broader native commit, redo/checkpoint reconciliation, and DDL/file-lifecycle
recovery work remain separate correctness and performance targets.
Ownerless latest-only checkpoint publication now also coalesces repeated
non-durable latest updates inside the same parser-proven implicit/autocommit
visible-fast append-batched statement after the first successful latest
checkpoint has been preserved. The final durable latest/visible checkpoint,
page-version WAL sync, explicit-transaction behavior, and unsafe ownerless
fault-test behavior are unchanged. Production probe output exposes the detailed
`checkpoint_update_deferred_latest_coalesced` counter plus compact autocommit
and bulk summary rows so CI can show whether capped multi-row inserts are
skipping redundant checkpoint updates.
The stats-enabled ownerless autocommit summary now also promotes the existing
native page-write detailed counters into per-insert summary rows for publish
calls, dirty-page scan work, deferred pages, tablespace lookup, scratch
allocation, page copy, checksum initialization, page-version hook time, and
commit-log subphases including flush-list, release, redo-leave, publish,
release-memo, and no-dirty-loop time. This keeps CI timing summaries useful
for the remaining write-path work without changing the page-version WAL format
or ownerless visibility semantics.
The production embedded performance probe also promotes existing ownerless
redo hook counters into compact autocommit and explicit-transaction summary
rows for enter, observe, reserve, written, and leave calls plus elapsed time,
so branch/main timing can distinguish redo-state hook work from checkpoint
updates, page-log append, and page-publication cost.
First-party database and embedded-open elapsed performance helpers now return
immediately when their stats-disabled zero sentinel is passed, matching the
earlier MariaDB page-write perf-helper cleanup. This keeps normal production,
WordPress PHPUnit, and stats-off embedded timing paths from paying an avoidable
second disabled-counter check while preserving enabled diagnostic counters.
Ownerless mini-transaction page-write paths now also cache transaction-release
classification inside the hot write-enter, page-publish, no-dirty commit-log,
and unlogged release loops. Statement-visible autocommit writes still use the
same page-write lock, refresh, publication, history-proof, and release rules,
but avoid repeated transaction-release and per-page transaction-deferral helper
work after the mini-transaction decision is known.
Modified-page entry and commit-publication sites then reuse the already-known
write-enter or `transaction_publish` decision when adding pages to
transaction-deferred dirty publishing. This preserves the same page-version and
history-proof publication rules while removing another redundant
classification hop from the ownerless write path.
The no-dirty page-write commit-log loop now also stops invoking the ownerless
page-write leave helper after the MTR-owned page-write vector has been
exhausted. Member page-write lock release, native memo release, page latch
release, page publication, redo handling, and checkpoint behavior are
unchanged; the fast path skips only helper calls that would have returned on an
empty vector.
The same no-dirty release loop then added a caller-side page-write membership
precheck while the vector is still non-empty, so non-member X/SX page memo
slots do not enter the ownerless leave helper only to return before release.
Member pages still use the same leave helper and release ordering before
native latch unlock.
The follow-up release-memo membership precheck applies that same non-empty
vector and page-membership guard to generic `mtr_t::release()` and
`release_unlogged()` ownerless page-write leave calls. The reduced 500-row
four-row-bulk production probe preserved `0.500` page versions per row,
`2.000` published native-support pages per statement, and `1.000`
commit-visibility fast path per statement, while bulk leave calls moved from
`1485` to `1458`, leave total from `2.101 ms` to `1.989 ms`, and release time
from `1.854 ms` to `1.685 ms`. Release-memo, no-dirty-loop, page-log append,
and throughput timings stayed noisy in short probes. This is a small
release-path cleanup; it does not reduce the remaining history/native proof
volume.
Ownerless page-write publication now snapshots the page-write performance
stats flag once per publish call and uses that snapshot for publish-call,
publish-total, subphase, and scratch-buffer reuse counters. Page publication,
history-proof marking, page-log append, checkpoint ordering, and recovery
semantics are unchanged; the change removes repeated diagnostics-only flag
loads from stats-disabled production paths.
Ownerless MTR page-publish loops now also open page-publish batch hooks lazily,
only when a pass reaches an immediate `ownerless_page_write_publish()` call.
Transaction-deferred dirty-page passes still record and capture deferred page
images in the same latch order, while deferred-only passes avoid the batch
begin/end hook pair. This is a hook-entry overhead reduction and does not
change page-version volume, native-support/history-proof publication,
page-log format, checkpoint ordering, or recovery semantics.
A local production stats-enabled sample after the lazy-batch slice preserved
explicit-transaction publication at `2` page versions and one page-log append
session begin/end pair per transaction, autocommit publication at `3.008` page
versions per insert and `2.004` native-support pages per insert, and the
four-row bulk shape at one page-log append session begin/end pair per
statement. The matching stats-off sample reported ownerless explicit
transactions at `0.6435x` ordinary, ownerless autocommit at `0.4558x`, and
ownerless four-row bulk rows at `0.4223x`.
Ownerless dirty transaction-page capture now reuses the transaction-publish
classification already computed by the collected-page publisher, full MTR memo
publisher, and no-dirty commit-log publish loop. The generic capture entry
point keeps its hook and transaction-publish checks, while preclassified
callers avoid recomputing that predicate before the capture helper revalidates
the transaction pointer, source page, transaction-owned page set, and page LSN.
This narrows explicit-transaction ownerless publish overhead without changing
dirty-page ownership, captured page images, page-version publication, WAL
format, checkpoint ordering, or recovery behavior.
The stats-enabled production sample after this slice preserved explicit
transaction publication at `2` page versions per transaction, `6` transaction
image publishes, `4` transaction buffer publishes, and one page-log append
session begin/end pair around the actual transaction page appends. Reduced
stats-off throughput samples were noisy, so this slice does not claim a stable
throughput win; the useful evidence is the removed duplicate predicate work
with unchanged publication counters.
Ownerless checkpoint LSN publication now skips rewriting the legacy
latest/visible payload once a valid checksummed LSN generation record already
exists. The legacy payload is still initialized for empty-record fallback,
generation records remain the authoritative recovery source, durable sync
ordering is unchanged, and performance summaries expose the
`checkpoint_update_legacy_write_elided` counter.
Ownerless page-log attribution summaries now also include append lock, fstat,
header/body setup, checksum, payload write, and record-header write time per
autocommit insert. These rows promote existing detailed counters so CI can
separate WAL encoding cost from positioned-write cost without changing page-log
record format or payload-before-header crash ordering.
The WordPress `perf-probe` now also compares process startup plus explicit
`mysqli_close()` against process startup plus implicit PHP object-free close
for a live mysqli link. Both paths intentionally continue through
`mylite_close()`; the added output keys only distinguish whether
process-isolated PHPUnit is paying a close-path shape that differs from the
existing explicit connect/close probe. The probe alternates the two close
shapes so one loop does not receive all warm filesystem/cache effects.
Those process-level `wordpress_perf_summary_*` keys are also appended to the
WordPress timing summary, alongside the in-process connect and SQL probe keys,
so CI step summaries show PHP startup, extension-load, explicit/implicit
process connect/close, and active in-process engine timings without log
scraping.
CI now also enables a separate, non-averaged process profile sample for one
explicit-close and one implicit-object-free WordPress mysqli process. Those
samples leave the existing process timing loop unprofiled, but append
`wordpress_perf_summary_mysqli_process_explicit_profile_*` and
`wordpress_perf_summary_mysqli_process_implicit_profile_*` rows for native
open/close attribution so the next process-isolated PHPUnit optimization can
target startup, close, or teardown with evidence.
The final WordPress timing rollup now preserves those explicit/implicit
process connect/close rows and native open/close profile rows, not just the
older combined connect/close alias, so the published timing artifact keeps the
per-process PHPUnit cost model visible for branch/main comparisons.
WordPress PHPUnit CI now splits the former visible
`non-isolated-query-canonical` timing tail into separate
`non-isolated-query` and `non-isolated-canonical` shards while keeping the
broad query/canonical and query/theme regexes as union and remaining-shard
exclusion authorities. This changes CI timing attribution and expected
wall-clock shape only; SQL, mysqli, native storage, and WordPress compatibility
semantics are unchanged.
WordPress PHPUnit CI also splits the former visible
`non-isolated-rest-content-controller` tail into separate
`non-isolated-rest-content-post` and `non-isolated-rest-content-support`
shards while keeping the broad REST content-controller regex as the
controller-other exclusion authority. This is another production timing
partition only; it does not change REST, SQL, mysqli, native storage, or
ownerless concurrency semantics.
The follow-up WordPress PHPUnit timing split also divides the visible
`non-isolated-query` shard into `non-isolated-query-filter` and
`non-isolated-query-core` while keeping the broad query and query/theme regexes
as union and remaining-shard exclusion authorities. This again changes only CI
timing attribution and expected wall-clock shape, not SQL, mysqli, native
storage, or ownerless concurrency behavior.
Ownerless mini-transaction page-write release now skips transaction lookup and
external release policy checks when the current MTR has no ownerless
page-write pages left to release. In the CI-shaped stats-enabled bulk probe,
this reduced ownerless bulk `page_write_leave_total_ms` from `3.892` to
`0.551`, kept bulk commit visibility on the fast path with zero publish
failures, and preserved native latch/memo release ordering. The no-new-dirty
commit-log path also skips page-publish batch setup when the mini-transaction
has no persistent modifications and skips page-write release checks when it
never acquired ownerless page-write state; a 200-row stats-enabled production
sample reduced ownerless autocommit `page_write_commit_log_no_dirty_loop_ms`
from `21.428` to `15.488`, `page_write_commit_log_publish_ms` from `30.745`
to `23.641`, and commit-MTR publish attribution from `0.154` to
`0.118 ms/insert`.
Ownerless page-version publication now skips synthesized snapshot-boundary
probes for native-support page classes and returns before the page-pin registry
latch when no active pins exist; normal page-version publication, history-proof
native-support records, active-reader retention, and boundary synthesis for
snapshot-sensitive pages remain unchanged. This is a bounded hot-path pruning
slice, while replacing or shrinking the remaining history-proof page
publication remains a planned performance target.
Ownerless native snapshot-boundary synthesis is now suppressed while a local
dictionary DDL statement is running and while the same handle remains in the
post-DDL conservative-write window under an external page-version pin. The
publish path still records external-lineage retention and current page-version
records, but it does not treat page LSN alone as proof that a newly created
file-per-table tablespace existed at an older reader snapshot. The same window
also suppresses external space-allocation refresh so local post-create
allocation pages are not refreshed from retained external state.
Ownerless page-version WAL now encodes repeated InnoDB `FIL_PAGE_INDEX`
records as bounded non-chained deltas against a durable standalone base record
when the page identity has warmed through standalone records and the delta is
less than half the standalone payload. Ownerless publish hook callers pass a
stable page image until the synchronous page-log append returns; current MTR,
dirty-scan, disk-replay, and transaction-image paths already pass copied,
vector-backed, or allocated page buffers. DDL-sensitive InnoDB
system-tablespace index pages stay on standalone encodings. Checkpoint
rewrites retained deltas as standalone records, and the process-local base
cache is scoped by page-log file identity, log offset, and log generation so
compaction or cross-process checkpoint generation changes cannot reuse stale
base offsets. The process-local base cache also forces a standalone base
refresh after a bounded delta run so long same-process writers do not keep
diffing against an old base indefinitely. A reduced stats-enabled production
probe over 100 ownerless autocommit inserts reported index page-log payload
falling from the preceding `1779` bytes/insert baseline to `598.580`
bytes/insert, with `90` delta records and `539.870` delta payload bytes/insert;
the follow-up 200-row base-refresh sample reported `619.275` index payload
bytes/insert and `496.030` index-delta payload bytes/insert, down from the
pre-refresh 200-row `217565` total index bytes and `207878` total index-delta
bytes. A later fast-path slice records each warmed base's standalone encoded
payload size and fast-accepts bounded small deltas before building a new
standalone payload; a 500-row production attribution sample reported `402`
fast-accepted index deltas out of `470` total index deltas and reduced
page-log append encode time from the preceding `54.815 ms` sample to
`30.980 ms` while preserving the same `1506` page-log append calls. The
first-base warm-up slice now lets one durable standalone index record seed
later bounded non-chained deltas; a reduced 500-row production attribution
sample reported index payload at `829.066` bytes/insert, down from the
preceding same-shape `927.956` bytes/insert sample, while retaining checkpoint
rewrite and the bounded base-refresh rule. Ownerless page-version WAL now also
uses the same non-chained durable delta shape for repeated `FIL_PAGE_UNDO_LOG`
records, with an undo-specific record flag, checkpoint rewrite to standalone
records, and separate attribution counters. A reduced 500-row production
attribution sample preserved one published rollback-segment history-proof page
and one published undo history-proof page per insert while undo-log payload
fell from the preceding same-shape `539.910` bytes/insert sample to `210.006`
bytes/insert, total page-log payload fell from `1442.294` to `1112.380`
bytes/insert, and `0.752` undo-delta records per insert were selected. Delta
base snapshots now retain immutable shared references to process-local cached
base pages instead of copying 16 KiB base images before each index or undo
delta encode; the durable non-chained delta format, base-record offset proof,
and checkpoint rewrite semantics are unchanged. A 500-row stats-enabled
production attribution sample after that change preserved `1506` page-log
appends, selected `481` index deltas and `376` undo deltas, and reported
`0.040` page-log encode ms/insert.
Ownerless page-version WAL can now encode the explicitly marked
rollback-segment `FIL_PAGE_TYPE_SYS` history-proof page with the same durable
non-chained delta shape after a standalone base record exists. The
rollback-segment page is still published as part of the existing two-page
native history proof, ordinary unhinted SYS pages remain standalone, and
checkpoint rewrite converts retained history-rseg deltas back to standalone
records. This is a payload reduction for the production ownerless hot path, not
a replacement proof or broad SYS-page delta policy.
Page-log scans now validate non-delta full, trailing-zero, sparse-zero,
compact sparse, varint compact sparse, and fill-sparse records by streaming the
logical full-page checksum instead of reconstructing a full 16 KiB page just to
prove payload integrity; delta records still use the existing base-page
reconstruction path because checkpoint rewrite depends on the full image.
Ownerless native-support page-version records now carry a page-log metadata
marker set by the publish hook after it classifies the page image. Native
checkpoint proof replay and oldest-snapshot boundary checks can skip payload
decoding for marked native-support records, while unmarked records fall back to
the previous page-type decode path. The marker is preserved by retained-record
checkpoint rewrites and does not replace the existing rollback-segment/undo
history proof or broader redo/checkpoint recovery work. Proof-only
native-support history-proof records extend that metadata path with no payload
to decode; they can prove publication ordering but cannot be replayed or read
as page images.
Ownerless page-log append batching and deferred latest-checkpoint coalescing
now stay enabled in unsafe hook builds unless an ownerless fault name is
actually configured with `MYLITE_OWNERLESS_TEST_FAULT`. The hook preset enables
fault infrastructure globally so crash tests can arm faults, but ordinary
focused correctness and performance selectors should still exercise the same
visible-fast append-batch path as production. When a named fault is configured,
batching and coalescing remain conservative so page-publish and checkpoint
fault windows stay individually observable.
Ownerless completion still requires the broader recovery, DDL/file-lifecycle,
active-reader, native history-proof replacement, and external stress gaps
tracked in the ownerless concurrency spec.
The timing-producing WordPress dependency, database-prep, performance-probe,
and PHPUnit test-only steps repeat `Release` MyLite and `MinSizeRel` MariaDB
embedded cache guards inside the step body, so a stale build directory fails
before it can publish misleading PHPUnit or perf-probe timings.
The WordPress PHPUnit harness now defaults to the same fast process-isolated
child mode used by CI: parent child-process profiling and defensive static
`wpdb` scanning are off unless a diagnostic run explicitly sets
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` or
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=1`.
CI process-isolated shards still enable
`MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`, which records parent-side
child count, lock-release, child runtime, and reconnect timings, plus
`MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY=1`, which records
child-script and outer-minus-script timings without turning on full
child-process profiling, static `wpdb` scanning, or mysqli aggregation.
A focused production smoke with child profiling off reported `7`
process-isolated children at `988.095 ms` runtime, `861.665 ms` child-script
time, `126.430 ms` outer-minus-script time, `6.925 ms` parent lock-release,
and `0.006 ms` reconnect per child, keeping CI's per-process startup and child
script cost visible without enabling the heavier diagnostic profile. The
WordPress timing rollup now also aggregates those process-isolated child
counts, runtime, script, outer-minus-script, lock-release, reconnect, and
baseline-restore rows, recomputing per-child averages from summed seconds and
counts so CI summaries do not require manual per-shard arithmetic.
The CI audit also forbids process-isolated timing steps from overriding
child-process profiling back on, and it requires the UI/filesystem shard to
keep parent reconnect disabled after each child, so their published wall
timings stay on the lean production path while per-child process timing remains
visible in the shared timing summary.
Ownerless statement startup now skips the heavier dictionary ready-wait path
when the handle has already observed the same stable idle dictionary
generation. Active dictionary DDL and changed generations still use the
existing wait and cache-refresh path; this is a statement-boundary overhead
reduction, not a change to dictionary generation semantics.
Ownerless direct and prepared statement startup now computes visible-fast
commit publication and page-log append-batch eligibility together. Eligible
`INSERT ... VALUES` statements resolve target foreign-key state once per
statement boundary while preserving the same dictionary-generation cache,
foreign-key blocking, append-row cap, and explicit-transaction commit proof.
The slow non-isolated WordPress PHPUnit CI timing step now keeps mysqli
profiling off by default, so the published test-only timing reflects the
production-built adapter path rather than diagnostic timer overhead. Diagnostic
runs can still set `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, which maps to
`MYLITE_MYSQLI_PROFILE=1` for that PHPUnit process and emits
`mylite_mysqli_profile_*` open/close, direct-query, result-query,
prepared-statement, cache, result-step, row-materialization,
status-synchronization, result-object, and fetch counters plus fetch elapsed
time from the production-built mysqli adapter; selected profile totals are
copied into the timing summary only for explicitly profiled phases. A focused
production `Tests_DB` run after the keepalive slice reported
`fetch_object_calls=76626` but only `fetch_object_ms_total=122.798`, while
`query_ms_total=19084.516`, `exec_no_result_ms_total=6329.991`,
`query_result_step_ms_total=4828.470`, `query_prepare_ms_total=4327.226`, and
`query_cache_clear_ms_total=3530.218`
remained much larger. The current WordPress performance target is therefore
MariaDB/libmylite query execution and prepared-statement lifecycle cost, not
PHP fetch-object conversion. Other WordPress CI timing steps keep that profile
disabled unless a diagnostic run explicitly opts in.
CI now includes that opt-in path as a separate `phpunit-db-profile` diagnostic
step after the normal unprofiled `phpunit-db` timing shard. The diagnostic step
reruns the bounded `^Tests_DB` filter with the same production build guards and
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, so the uploaded WordPress timing
summary has default profile attribution rows without adding profiler overhead
to the normal database, process-isolated, or non-isolated timing shards. The
current local production diagnostic sample passed 651 tests with 3 skips and
reported `query_ms_total=5290.576`, `query_verb_select_ms_total=2589.649`,
`query_verb_transaction_ms_total=1584.162`, and
`libmylite_exec_result_native_control_ms_total=1572.866`, while the previously
documented transaction-end helper remains rejected because it regressed the
same focused profile.
The same opt-in profile now also prints bounded top SQL-shape rows for text
queries. Samples normalize literal strings and numbers to `?`, collapse
whitespace, and report hash, call count, elapsed time, verb, and sample text for
the top profiled shapes. The WordPress timing summary copies those rows only
for profiled phases, so normal test-only timings remain comparable while the
diagnostic database shard can identify the next high-impact query family.
The same opt-in profile now also emits `libmylite_exec_result_*` rows that
separate direct text execution into native `mysql_query()`, affected-row and
insert-id capture, result draining, result/no-result classification,
current-schema update, handle status-update buckets, and native-control
fast-path calls. These rows are diagnostics for profiled WordPress and mysqli
runs only; they do not change SQL semantics, public C API compatibility, native
storage behavior, or default CI timing overhead.
The embedded ownerless attribution probe now reuses those existing
exec-result counters for ordinary and ownerless bulk autocommit insert phases,
emitting both raw `*_exec_result_*` rows and compact per-statement
`mysql_query()`, result-drain, status-update, and call-count summaries. This is
diagnostic-only and is intended to expose whether the remaining bulk insert
gap sits inside SQL text execution or the already-reported page-log,
page-write, commit-visibility, SQL-handler, and InnoDB-handler buckets.
Ownerless production mini-transaction redo completion now has an optional fused
written-plus-leave hook for top-level redo ranges. The path keeps the existing
separate `redo-written` and `redo-leave` callbacks as the fallback and leaves
unsafe named-fault hook builds on the separate callbacks, but production can
complete the reserved redo range and publish the latest LSN through one shared
redo-state progress-latch pass. The pre-slice 5000-row, 100-row-per-statement
attribution baseline reported ownerless bulk at `18594.73 rows/s`, ordinary
bulk at `108178.15 rows/s`, ownerless `mysql_query()` at `4.895 ms` per
statement, and commit-log redo-leave at `0.988 ms` per statement. The
post-slice sample reported ownerless bulk at `19629.59 rows/s`, an
ownerless/ordinary ratio of `0.2052`, ownerless `mysql_query()` at
`4.614 ms` per statement, page-write commit-log at `1.382 ms` per statement,
and commit-log redo-leave at `0.737 ms` per statement. This is tracked as a
bounded hot-path reduction rather than a full ownerless completion claim.
Ownerless redo state now keeps an active-reservation counter in the shared
redo-state segment, bumping that segment descriptor from version `8` to `9`.
Reserve and complete-write paths update the counter while holding the existing
redo progress latch, and leave/snapshot policy reads the counter directly
instead of scanning the 64 reservation slots each time it checks whether a
latest-LSN advance can also advance the written LSN. SQL behavior, WAL format,
checkpoint files, and page-version publication are unchanged. A reduced
production stats-off 30000-row, 100-row-per-statement probe reported ownerless
bulk at `21802.61 rows/s`, ordinary bulk at `95543.15 rows/s`, and an
ownerless/ordinary ratio of `0.2282`; this is recorded as a bounded redo-state
bookkeeping reduction, not as closure of the broader ownerless performance
gap.
The next redo-state bookkeeping slice keeps a completed-range counter in the
remaining unused bytes before the progress latch, bumping the redo-state
segment descriptor from version `9` to `10`. Out-of-order range recording,
merging, and draining maintain the counter under the existing progress latch,
and sequential contiguous redo completion now returns from empty
completed-range drains without scanning the completed-range slot table. LSN
ordering, out-of-order range coalescing, checkpoint files, page-version WAL,
native InnoDB files, and SQL behavior are unchanged. The final same-preset
stats-enabled 100-row bulk attribution sample kept `2.000` page-version records
and `4.500` page-log append calls per statement while commit-log redo-leave
time moved from `0.743 ms` to `0.687 ms` per statement; the final stats-off
5000-row, 100-row-per-statement sample reported ownerless bulk at
`24510.00 rows/s`, ordinary bulk at `88307.92 rows/s`, and an
ownerless/ordinary ratio of `0.2776`. This remains a bounded redo-state
bookkeeping reduction, not a full ownerless write-throughput fix.
The redo-leave subphase attribution follow-up then splits the existing
`page_write_commit_log_redo_leave` bucket into native `log_write_up_to()` time,
MyLite redo-state hook time, written-range hook calls, fallback hook calls, and
zero-LSN leaves without changing call order or redo/checkpoint behavior. A
reduced 100-row bulk attribution sample preserved `2.000` page versions,
`4.500` page-log appends, `2.000` native-support published pages, fast commit
visibility, and `180.500` deferred latest-checkpoint coalesces per statement
while reporting `0.671 ms/statement` aggregate redo leave, split into
`0.362 ms/statement` native log write and `0.281 ms/statement` MyLite hook time;
all `181.500` redo leaves per statement used the written-range hook, with zero
fallback or zero-LSN leaves.
Ownerless MTR page-write release now has a known-page helper for release loops
that have already proven a memo slot is tracked by the current MTR. Generic
release paths keep the defensive membership checks; the proven release loops
skip only the repeated lookup before using the same forget/release logic. A
reduced 100-row bulk probe preserved `2.000` page versions, `4.500` page-log
appends, `2.000` native-support published pages, and fast commit visibility
while reporting noisy post-change no-dirty page-leave samples between
`0.204` and `0.220 ms/statement`. This is a bookkeeping cleanup, not a new
compatibility claim or a broad throughput claim.
Ownerless history-proof pair publication now uses a page-log append-session
fast path for the two existing proof-only native-support records when the
normal append session is active. The fast path writes the same two 64-byte
zero-payload WAL headers, preserves record-count append/session counters,
coalesces the adjacent proof-only headers into one physical record-header write
when the pair API is used, and falls back to the previous two generic appends
when session setup is unavailable. Primitive coverage proves the pair API keeps
proof records unreadable as page images and skipped by replay/checkpoint
callbacks, while focused history-proof SQL still proves successful pair
publication in normal builds. A reduced stats-off 5000-row,
100-row-per-statement production sample
reported ownerless bulk at `24520.62 rows/s`, ordinary bulk at
`81550.84 rows/s`, and a `0.3007` ownerless/ordinary ratio; this is a bounded
append-path cleanup, not a broad throughput or recovery-completion claim.
Ownerless visible-fast statements now also batch mini-transaction redo
completion inside the existing deferred page-publication boundary. The
production path defers top-level `(start_lsn, end_lsn, latest_lsn)` completion
to a bounded thread-local batch, writes native redo once at the flush boundary,
then completes each range through the existing fused written/leave hook before
page-visible publication, statement teardown, or hook reset. Unsafe hook builds,
non-visible-fast statements, nested redo, missing callbacks, and DDL/file
lifecycle paths stay on the conservative immediate path. SQL behavior, public
APIs, native redo format, page-version WAL, checkpoints, and directory layout
are unchanged. A reduced stats-enabled 100-row bulk attribution sample reported
ownerless `mysql_query()` at `3.406 ms` per statement, page-write commit-log at
`0.725 ms` per statement, commit-log redo-leave at `0.260 ms` per statement,
zero immediate page-write `log_write_up_to()` calls, `162.000` written/leave
events per statement, and zero fallback hook calls. The matching stats-off
5000-row, 100-row-per-statement production sample reported ownerless bulk at
`27297.54 rows/s`, ordinary bulk at `96781.94 rows/s`, and a `0.2821`
ownerless/ordinary ratio. This is a bounded visible-fast hot-path reduction,
not completion of the broader ownerless redo/checkpoint or DDL/file-lifecycle
recovery work.
Ownerless visible-fast redo completion now also batches the shared redo-state
written/leave work behind that same flush boundary. The InnoDB hook layer
passes deferred ranges to an optional batch callback, and the first-party redo
state completes the written ranges and matching owner leaves while holding the
progress latch once. The single-range fused callback remains the fallback for
missing batch callbacks, unsafe hook builds, and non-deferred paths. SQL
behavior, public APIs, native redo/page-version formats, checkpoints, and
directory layout are unchanged. A reduced stats-enabled 100-row bulk
attribution sample reported database redo written/leave callbacks falling from
`810` to `29` across five statements, while logical page-write written/leave
events stayed at `162.000` per statement and fallback hook calls stayed at
zero. Commit-log redo-leave moved from `0.260 ms/statement` to
`0.094 ms/statement`, page-write redo hook time moved from
`0.243 ms/statement` to `0.079 ms/statement`, and ownerless `mysql_query()`
remained `3.405 ms/statement`. The matching stats-off 5000-row,
100-row-per-statement production sample reported ownerless bulk at
`33432.91 rows/s`, ordinary bulk at `107188.95 rows/s`, and a `0.3119`
ownerless/ordinary ratio. The normal-build positive assertion for deferred
latest-checkpoint coalescing in the explicit-transaction undo WAL-elision case
was removed because redo latest/checkpoint advancement is now allowed to happen
at the batch boundary rather than through that older coalescing counter.
The deferred redo range cap now rises from `32` to `48` ranges, still below
the `64` shared active-reservation slots used by the ownerless redo state. A
full-range embedded hook test proves one `48`-range batch callback and full
completed-count reporting before page-visible publication. The matching
16384-row production probe preserves logical page-write redo event volume
while moving database-level redo written/leave callbacks from `1029`/`1028` to
`686`/`685`, page-write redo hook time from `6.060` to
`5.703 ms/statement`, and ownerless/ordinary bulk ratio from `0.6971` to
`0.7717`; this remains a bounded callback/latch reduction, not completion of
broader native row/undo, redo/checkpoint, or group-commit work.
The deferred redo range cap now rises again from `48` to `61` ranges. The cap
stays below the `64` shared redo-state slots after accounting for the current
active-owner entry and two peer headroom slots; primitive coverage proves a full
configured batch still allows a newly arriving peer to enter and reserve one redo
range before the original batch flushes, and embedded hook coverage proves one
full `61`-range batch callback with full completed-count reporting. The accepted
100-row stats-enabled page-publish attribution probe reported database redo
written/leave callbacks at `39`/`38` across ten bulk statements while preserving
`181.800` logical page-write written-hook events per statement, `2.000`
page-version records per statement, and `32.300` page-log append calls per
statement; stats-off local samples remained noisy, so this is a bounded
callback/progress-latch reduction rather than a broad throughput claim.
The follow-up bulk engine attribution slice keeps the same runtime semantics
but enables ordinary bulk SQL-handler, InnoDB-handler, and deep InnoDB counters
under the existing stats-enabled production probe. The probe now emits raw
ordinary bulk engine rows and compact ownerless-minus-ordinary bulk deltas for
native commit, history write, row insert, clustered insert, undo report, and
default-checked bulk-start buckets in both per-row and per-statement form. This
is diagnostic-only: it does not change SQL behavior, page-version publication,
redo/checkpoint ordering, WAL format, or recovery, and it does not claim a
throughput win.
The later undo attribution follow-up extends those first/remaining bulk rows
to the existing B-tree lock/undo and undo-report subcounters. A reduced
2048-row production probe reported later ownerless statements at `20.857 ms`
per statement in the B-tree lock/undo undo-report subphase, `20.606 ms` in
undo-report total, `13.523 ms` in undo-report MTR commit, and `4.671 ms` in
persistent undo assignment while preserving `2048.000` undo-report successes
and `0.000` default-checked bulk starts per statement. A one-statement guard
reported zero remaining-statement averages, so the phase split does not reuse
aggregate counters when no later statement exists.
Ownerless native-support page publication now has a stats-off production fast
skip for pages that the existing native-support rules would only elide. The
fast path is disabled whenever page-publish or page-write diagnostics are
enabled, requires the page image LSN to match the mini-transaction commit LSN,
and reuses the existing history-proof/native-support elision predicate. It does
not skip native undo records, page-write locking or release, redo completion,
transaction-deferred user page publication, or active rollback-segment/undo
history-proof pages.
The native-support lock-hold follow-up keeps those safety boundaries and
reduces repeated page-write lock churn only by holding actual shared
page-write locks until autocommit transaction cleanup for native-support pages
that the same elision predicate already accepts. It records those held pages in
a separate transaction-local vector rather than the modified/dirty vectors, so
commit-time page-version publication and visible-fast proof logic are
unchanged. Focused SQL coverage proves native-support WAL elision still occurs
and that visible-fast multi-row inserts reuse a held native-support page-write
lock inside one statement. A reduced stats-enabled 2048-row production probe
reported `47` held native-support page-write locks and `36947` already-held
hits in the ownerless bulk phase; the companion stats-off sample reported
ownerless 2048-row bulk rows at `63087.14 ops/s` versus ordinary at
`125321.71 ops/s`, ratio `0.5034`.
The held native-support publish-skip follow-up then avoids a redundant
page-publication helper dispatch for pages already present in the
transaction-local held native-support page-write list, while still requiring
the full helper when page-publish diagnostics are enabled or a rollback-segment
or undo history-proof role is active. The production performance probe now has
`MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1` to expose page-write counters without
page-publish stats. A reduced 1000-row page-write-only bulk probe reported
`20` held native-support page-write locks, `1822` already-held hits, and `901`
held-native-support publish skips, while a page-publish stats probe preserved
`2.000` published native-support proof pages, `100.000` elided native-support
pages, and `0.000` held-publish skips per remaining 100-row statement.
The transaction page last-hit cache follow-up keeps those authoritative
modified, dirty, and held native-support page vectors plus their lazy exact
sets unchanged, but remembers the last exact positive membership result for
each class inside `trx_t`. Repeated hot-page checks in ownerless non-empty
bulk row/undo mini-transactions can return from that process-local cache, while
misses, vector/set clear and rebuild paths, page-write lock ownership,
page-version publication, native undo/redo, and recovery semantics stay on the
existing paths. This is a bounded hot-path cleanup, not a broad non-empty-table
undo elision or ownerless completion claim.
The held native-support hit fast path now moves that already-held
native-support membership check earlier in ownerless page-write prepare/enter
classification. Repeated undo and rollback-segment mini-transactions that hit a
native-support page already retained by the visible-fast transaction can return
before broader transaction-release classification, while the authoritative
held-page vector, native-support elision predicate, rollback-segment/undo
history-proof publication, page-version WAL, redo/checkpoint ordering, and
non-empty-table rollback semantics stay unchanged.
The reduced 100-row stats-enabled attribution sample kept `2.000` page
versions and `2.000` published native-support pages per remaining statement,
while earlier prepare classification reduced reported enter-path held hits from
`202.000` to `102.000` per remaining statement. In that local sample,
ownerless bulk `mysql_query()` moved from `3.431 ms/statement` to `2.912`, and
the ownerless bulk rows ratio moved from `0.2966` to `0.4640`; the matching
stats-off 5000-row production probe reported ownerless bulk at `34064.01`
rows/s and a `0.3391` ownerless/ordinary ratio. Remaining undo-report MTR
commit time is still a larger native undo target.
The bulk page-write phase split then keeps the same diagnostics-only boundary
but snapshots the first row-list statement for page-publish, database hook,
page-write, page-log append, and commit-visibility counters. A reduced
two-statement 100-row production probe reported the later non-empty-table
statement at `0.438 ms` in page-write commit-log time, split into
`0.105 ms` redo leave, `0.178 ms` commit-log publish, and `0.244 ms`
no-dirty-loop time per statement, while a one-statement guard emitted `0.000`
for remaining page-write phase rows.
Profiled mysqli runs also split total query elapsed time into
`query_verb_*` buckets for result queries, DML, DDL, connection state,
transaction, lock, call, and other first-keyword classes so WordPress timing
summaries can identify which SQL class dominates native execution time before
choosing an optimization target. The transaction bucket is further split into
`query_transaction_start_*`, `query_transaction_end_*`, and
`query_transaction_savepoint_*` profile rows so production WordPress PHPUnit
timing summaries can separate `BEGIN`/`START`, `COMMIT`/`ROLLBACK`, and
`SAVEPOINT`/`RELEASE` cost while preserving the existing aggregate
`query_verb_transaction_*` totals.
Non-ownerless direct execution now fast-paths exact simple `START TRANSACTION`,
`COMMIT`, full `ROLLBACK`, and `SET autocommit = 0|1` text statements through the
native-control result path after the query-verb profile showed repeated
WordPress PHPUnit transaction scaffolding dominating native `mysql_query()`
time. In embedded MariaDB, `mysql_commit()`, `mysql_rollback()`, and
`mysql_autocommit()` are still public C API wrappers around
`mysql_real_query()`, so this path avoids MyLite's outer result/status work for
those exact wrappers. Exact `START TRANSACTION` uses a MyLite-owned embedded
helper that calls MariaDB's `trans_begin(thd, 0)`, finalizes through the
embedded OK/error protocol, and reads the result back into `MYSQL`.
`COMMIT` and `ROLLBACK` still use MariaDB's public embedded wrappers after a
native transaction-end helper prototype failed the performance gate, preserving
MariaDB parser-owned `completion_type`, `AND CHAIN`, and `RELEASE` behavior.
Option-bearing forms such as `START TRANSACTION READ ONLY` or
`START TRANSACTION WITH CONSISTENT SNAPSHOT`, plus `COMMIT AND CHAIN`,
`ROLLBACK TO SAVEPOINT`, and general `SET` expressions still use MariaDB SQL
parsing, and ownerless execution keeps the existing statement-lock,
page-version, and publication path. A focused production profiled `^Tests_DB`
sample after the fast path reported `libmylite_exec_result_native_control_calls=1310`,
`libmylite_exec_result_native_control_ms_total=1404.955`,
`libmylite_exec_result_mysql_query_ms_total=3946.376`,
`query_ms_total=5432.945`, and
`wordpress_phpunit_reported_seconds=7.914`; the preceding query-verb sample
reported `libmylite_exec_result_mysql_query_ms_total=6891.775`,
`query_ms_total=6979.728`, and `wordpress_phpunit_reported_seconds=9.596`.
A follow-up production profiled `^Tests_DB` sample with transaction sub-buckets
reported `query_transaction_start_calls=651`,
`query_transaction_start_ms_total=708.722`,
`query_transaction_end_calls=659`, `query_transaction_end_ms_total=705.569`,
`query_transaction_savepoint_calls=0`, and
`wordpress_phpunit_reported_seconds=8.195`, showing the remaining transaction
cost is split nearly evenly between transaction start and transaction end in
that workload.
The autocommit no-op fast-path follow-up now skips exact repeated
`SET autocommit = 0|1` statements in non-ownerless direct execution when
`MYSQL::server_status` already reports the requested autocommit state, while
changed-state forms still call MariaDB's `mysql_autocommit()` wrapper and
ownerless execution stays on the existing SQL/locking path. Profiled mysqli
and WordPress timing summaries now include
`libmylite_exec_result_native_control_autocommit_noops`; the
start-transaction fast-path follow-up also exposes
`libmylite_exec_result_native_control_start_transaction_calls`. The
transaction-end profile follow-up exposes
`libmylite_exec_result_native_control_commit_calls`,
`libmylite_exec_result_native_control_rollback_calls` so profiled runs can
distinguish exact transaction-end volume from starts and autocommit controls
without changing transaction-end execution.
The accepted transaction-end profile rerun of focused production WordPress
`^Tests_DB` passed 651 tests with 3 skips and reported
`libmylite_exec_result_native_control_start_transaction_calls=651`,
`libmylite_exec_result_native_control_commit_calls=8`,
`libmylite_exec_result_native_control_rollback_calls=651`, and
`libmylite_exec_result_native_control_errors=0`,
`query_transaction_start_ms_total=828.318`,
`query_transaction_end_ms_total=810.402`, and
`wordpress_phpunit_reported_seconds=8.314`, keeping transaction-end work
visible while leaving MariaDB's wrapper path intact. The focused
production WordPress `^Tests_DB` run for the start-transaction follow-up passed
651 tests with 3 skips and reported
`libmylite_exec_result_native_control_start_transaction_calls=651`,
`query_transaction_start_ms_total=725.444`,
`query_transaction_end_ms_total=707.323`,
`libmylite_exec_result_native_control_ms_total=1426.880`, and
`wordpress_phpunit_reported_seconds=8.252`, keeping transaction-start cost near
the prior measured range while making the fast-path hit count visible. The
previous autocommit no-op focused production WordPress `^Tests_DB` run passed
651 tests with 3 skips and reported
`libmylite_exec_result_native_control_calls=1310`,
`libmylite_exec_result_native_control_autocommit_noops=644`,
`libmylite_exec_result_native_control_ms_total=742.014`,
`query_ms_total=5207.110`, and `wordpress_phpunit_reported_seconds=7.798`.
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
statements without `RETURNING`, while dropping the immediate-repeat promotion
marker so post-DML result queries still execute against current rows. It can
also preserve both the prepared result cache and the immediate-repeat promotion
marker across exact `START TRANSACTION`, `BEGIN`, `COMMIT`, and `ROLLBACK`
controls, so eligible repeated result queries around transaction scaffolding can
still promote into cached prepared execution. Conservative cache clears remain
for DDL, schema, lock, `SET`, `USE`, `CALL`, non-exact transaction forms,
reconnect, close, and error paths. The earlier DML cache-retention focused
production `Tests_DB` sample reported
`query_cache_preserved_no_result_calls=111`, `query_cache_hits=3`,
`query_prepare_calls=1612`, `query_cache_clear_finalize_calls=1612`,
`query_ms_total=15296.434`, and `exec_no_result_ms_total=6775.758`, with
`wordpress_phpunit_reported_seconds=19.314`; the previous focused attribution
sample reported `query_cache_hits=0`, `query_prepare_calls=1615`,
`query_cache_clear_finalize_calls=1615`, `query_ms_total=19084.516`, and
`wordpress_phpunit_reported_seconds=26.509`.
The transaction-control cache-retention focused production `Tests_DB` rerun on
the current default text-result route passed `651` tests with `3` skips and
reported `query_cache_preserved_no_result_calls=1421`,
`query_cache_hits=0`, `query_prepare_calls=0`,
`query_cache_lookup_calls=0`, `query_ms_total=6948.686`,
`exec_no_result_ms_total=3675.894`,
`libmylite_exec_result_native_control_ms_total=1747.709`, and
`wordpress_phpunit_reported_seconds=10.064`, confirming that the remaining
focused database-shard cost is native query and transaction execution rather
than prepared-result cache lookup overhead. Full non-isolated shard timings
remain the authority for suite-wide impact.
The WordPress timing rollup now also reports wall-clock critical-path evidence:
the green `1f8313b0` production run showed setup at `476s`, slowest shard
`non-isolated-rest-controller` at `182s`, estimated workflow critical path at
`658s`, and slowest PHPUnit test-only shell time at `140.722s`, while summed
parallel non-isolated test work was `552.648s`.
The later green `27896266558` run, after splitting content/media from
user/auth, reported setup at `74s`, slowest shard
`non-isolated-rest-controller` at `186s`, estimated workflow critical path at
`260s`, and slowest PHPUnit test-only shell time at `147.060s`, which is the
evidence for the REST controller tail split.
The green `27896697422` run after that split reported setup at `78s`, slowest
shard `non-isolated-remaining` at `158s`, estimated workflow critical path at
`236s`, and slowest PHPUnit test-only shell time at `121.495s`, making the
broad remaining and content/media shards the next timing fanout target.
The green `27897052195` run after that fanout reported setup at `82s`, slowest
shard `non-isolated-query-theme` at `147s`, estimated workflow critical path
at `229s`, and slowest PHPUnit test-only shell time at `105.802s`, making
query/theme the next timing split target.

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
queries while retaining conservative invalidation points for DDL, schema, lock,
`SET`, `USE`, `CALL`, non-exact transaction forms, explicit prepared
statements, reconnect, close, and error paths. Exact transaction-control
statements preserve cached result statements. The profile test covers
non-consecutive exact SELECT reuse in addition to DML-preserved current-row
visibility. The focused production `Tests_DB` profile after this LRU slice
reported
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
embedded test steps. It also guards the CI performance artifact names and
paths so timing evidence remains downloadable when future workflow edits touch
the performance jobs. The WordPress timing rollup also reports the critical
shard's artifact download, artifact extraction, Docker image, PHPUnit total,
PHPUnit shell, and non-PHPUnit-shell seconds so CI can distinguish fixed shard
setup cost from test-body and engine execution time.
Guarded ownerless SQL page-version reads are now enabled only when statement
refresh actually needs page-version WAL. In a continuous single-owner epoch,
local autocommit writes advance a separate local-native read boundary; eligible
same-runtime reads covered by that boundary avoid shared page-version pins and
the InnoDB file-read overlay. Autocommit writes outside that proof still seed
the real page-version read LSN for multi-process read-your-writes and retained
refresh. The active-reader pressure case covers the direct AUTO_INCREMENT
read-before-DDL shape that exposed the accidental overlay.
Ownerless page-version WAL records now carry an internal external-snapshot
lineage marker when they are written by a runtime that consumed WAL retained
for another ownerless reader's snapshot. No-live native checkpoint proof may
accept a newer file-per-table page only for marked records whose native disk
LSN is still within the visible reclaim boundary; unmarked commit-race records
continue to require exact native proof or replay.
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
from full-page WAL write volume. The same probe now also attributes
`FIL_PAGE_INDEX` page-log identity reuse and changed-byte density with raw and
per-insert counters for unique, duplicate, size-mismatch, overflow, total
changed bytes, `FIL` header changed bytes, and body changed bytes. The reduced
100-row production sample after adding the index delta attribution reported
`0.990` duplicate index identities per ownerless autocommit insert, no
identity size mismatches or overflows, only `39.860` changed bytes per insert,
and `1779.010` index payload bytes per insert. This makes a bounded index
delta WAL format the next evidence-backed payload target.
The prepared DML reset path now avoids
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
The same production attribution mode now also enables existing ownerless
database and direct-exec counters around ownerless direct and prepared
`SELECT 1` probe sections, reporting per-select direct `mysql_query()` timing,
prepared `mysql_stmt_execute()` timing, prepared ownerless stage timing,
page-version read-hook counts, and WAL-scan time. A reduced 25-select
production attribution sample reported zero page-version read hooks and zero
WAL-scan time for both direct and prepared tableless reads, with direct
`mysql_query()` at `0.220 ms/select` and prepared step total at
`0.206 ms/select`, of which `0.202 ms/select` was native MariaDB execute time.
The probe now also times a real InnoDB primary-key point select in ordinary and
ownerless direct/prepared forms and, under the same attribution flag, reports
ownerless point-select direct exec stages, prepared-step stages, and
page-version read-hook counts/time. These counters separate tableless probe
overhead from the real-table reads that dominate application suites such as
WordPress PHPUnit. The first reduced 25-select point-select sample reported
ownerless/ordinary direct and prepared ratios of `0.5085` and `0.4280`, zero
page-version read hooks, direct `mysql_query()` at `0.447 ms/select`, and
prepared step total at `0.497 ms/select`, of which `0.038 ms/select` was
ownerless refresh and `0.443 ms/select` was native MariaDB execute.
The follow-up native hook attribution slice extends those same stats-enabled
read windows with ownerless MDL, transaction, and read-view callback counts and
timing so real-table read overhead can be separated from page-version reads and
statement-boundary refresh before any read-policy optimization. A reduced
100-select sample reported zero such hooks for tableless reads and, for
point-select reads, roughly one MDL acquire/release, one transaction snapshot,
and one read-view register/deregister per select, with about
`0.008-0.011 ms/select` in those hook callback bodies versus
`0.281-0.312 ms/select` in native point-select execution.
The follow-up refresh attribution slice then split the statement-boundary
refresh aggregate: a reduced 100-select sample reported tableless reads doing
no shared snapshot, page-version pin, clean-page refresh, visibility, or native
flush work, while ownerless direct/prepared point selects spent
`0.035-0.036 ms/select` in refresh and `0.034-0.035 ms/select` of that in the
shared redo/process/transaction snapshot before selecting the
local-native-current-read path.
The single-owner refresh-snapshot fast path now uses process active
count/generation to avoid scanning other process and transaction slots when the
current process is the only ownerless owner. In a reduced stats-enabled sample,
point-select refresh dropped to `0.002 ms/select` with only
`0.001 ms/select` in the shared snapshot; a 1000-select stats-off sample
reported ownerless direct point-select ratio `0.9028` and prepared point-select
ratio `0.7707`, leaving prepared native execute overhead as the next read-path
target.
Point-select engine attribution now adds opt-in production-probe counters at
the SQL handler `ha_index_read_*()` boundary and InnoDB `index_read()` child
stages. A reduced stats-enabled sample reported no page-version reads, no
per-step native reprepare, ownerless prepared-step total at
`0.313 ms/select` with `0.303 ms/select` in native execute, and InnoDB
`row_search_mvcc()` timing of about `0.012 ms/select` for ownerless direct and
prepared point selects versus `0.006 ms/select` for ordinary point selects,
keeping the next optimization target inside native row-search/read-view
lifecycle rather than MyLite wrapper setup. A 3000-select stats-off sample from
the same build reported tableless direct/prepared ratios of `0.9221`/`0.8666`
and point-select direct/prepared ratios of `0.8475`/`0.8276`.
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
occurred. When another ownerless peer is live, `INSERT ... VALUES` with an
explicit target-column list into an AUTO_INCREMENT target stays on the
conservative refresh/flush bridge because the current proof does not merge
already-local dirty leaf pages with a peer's committed insert batch. `INSERT ...
ON DUPLICATE KEY UPDATE`, `INSERT ... RETURNING`, and `INSERT ... SELECT` stay
on the conservative unproven-statement bridge. The
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
embedded `build`, `measure`, or warmed `ensure` output. The WordPress MariaDB
embedded archive cache key is scoped to MariaDB source, the embedded profile,
and `tools/mariadb-embedded-build`, so harness-only timing/sharding edits do
not evict a valid production archive cache. The same probe now
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
time. The same stats-enabled attribution probe also emits ownerless
record-lock wait-until calls, elapsed time, and OK/timeout/unavailable/error
result classes, with bulk first/remaining splits, because the insert-intention
availability probe is distinct from explicit record lock acquire/release
publication. The final local 16K-row production attribution sample reported
`16384` ownerless bulk record wait-until calls, `24.478 ms` total, all OK, and
zero timeout/unavailable/error results; in the remaining bulk statement this
accounted for most of the `29.046 ms` ownerless record-lock bucket, but the
larger remaining deltas were still row insert (`190.280 ms`), undo report
(`76.220 ms`), undo-report MTR commit (`70.608 ms`), and page-write commit-log
work (`58.450 ms`) per statement. A follow-up single-owner record-wait skip now
returns OK before consulting the shared record-lock registry only while the
process registry proves `active_count == 1` and generation equality with the
current owner slot and the record-lock registry has no waiting entries or
foreign active record owner. Same-process InnoDB record-lock checks still run
before the callback, live-peer or stale-generation cases keep the shared
registry probe, and a corrective product-hook check keeps synthetic external
record locks visible in the shared wait table. The final 16K-row production
probe after the skip reported the same `16384` ownerless bulk wait-until calls
with `16384` allowed single-owner skips, reducing wait-until time to
`1.098 ms` per remaining statement; a later corrective reduced probe after the
record-registry guard still reported `16384` allowed skips and `1.723 ms`
remaining record wait-until time. The same sample reported ownerless bulk at
`72537.54 rows/s`, an ownerless/ordinary ratio of `0.7891`, and
remaining-statement row insert, undo report, undo-report MTR commit, and
page-write commit-log costs still at `285.786 ms`, `104.685 ms`,
`77.828 ms`, and `58.187 ms`. The same stats-enabled attribution
probe also emits ordinary insert
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
production attribution sample then showed that detailed page-log page-type
classification alone can dominate stats-enabled append timing; the embedded
performance probe now defaults that detail work off and prints
`mylite_perf_ownerless_page_log_detail_stats=0`, while
`MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1` restores page-class and
index-identity buckets for byte-composition investigations. This is a
diagnostics overhead split only: stats-off production behavior, page-log
records, page-log sync, recovery, and checkpoint rules are unchanged.
A later bounded local stats-enabled sample after adding the visible-anchor
split reported page-visible hook cost at `0.012 ms` per ownerless autocommit
insert, while the broader checkpoint update primitive ran four times per
insert with `0.137 ms` total, mostly current-LSN read time, and native
clustered/undo mini-transaction deltas remained larger;
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
`wpdb` before each child; on the measured production host, the
`Tests_Formatting_Emoji` class changed from `41.816s` shell real with full
static scanning to `31.599s` with static scan disabled, and a same-session A/B
with static scan disabled measured `28.711s` shell real with child profiling
disabled versus `30.354s` with child profiling enabled. The harness exposes
`MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD`, defaulting to eager
reconnect, so CI skips parent reconnect only for exact process-isolated filters
proven under production builds. Earlier broad deferred trials failed
parent-side `wpdb` reconnection checks, so CI first limited deferred reconnect
to verified subsets; the current split keeps the factory-heavy deferred shard
baseline-restored, the skip-safe deferred shard child-install-only, and the
UI/filesystem shard reconnect-disabled after a fresh production run passed the
same `22` tests with parent reconnect at `0.009 ms` per child. An earlier mysqli
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
A later reduced production close-attribution probe with the same guarded
WordPress build shape reported explicit process connect/close at `759.288 ms`,
implicit PHP object-free process connect/close at `617.743 ms`, and
active-runtime reconnect at `3.644 ms`, keeping the process-isolated slowdown
attributed to full embedded lifecycle startup/shutdown rather than an
avoidable implicit PHP destructor penalty.
A reduced embedded production probe on 2026-06-17 after the ordinary startup
gate reported ordinary warm open/close at `474.221 ms`, including
`165.433 ms` in `mysql_server_init()` and `301.127 ms` in
`mysql_server_end()`. The same ordinary sample reported zero time in ownerless
concurrency metadata, shared-memory preparation, shared-memory mapping,
page-log open, checkpoint open, redo evidence, close-time ownerless reclaim,
redo capture, and unmap phases, proving fresh ordinary opens no longer create
or map ownerless coordination files. Ordinary active-runtime reconnect stayed
cheap at `0.829 ms`. The remaining process-isolated cost is therefore MariaDB
embedded lifecycle startup/shutdown, not ownerless SHM/WAL/checkpoint setup.
The embedded shutdown attribution probe now emits `release_mysql_thread_end`
and `release_mysql_server_end` alongside the retained
`release_mysql_shutdown` aggregate so future shutdown work can target the
MariaDB cleanup side only when measured evidence points there. A follow-up
cleanup-attribution probe on 2026-06-18 split `mysql_server_end()` and reported
`219.581 ms` in `clean_up()`, of which `219.017 ms` was
`plugin_shutdown()`; the process-isolated optimization target is therefore
MariaDB plugin teardown, not MyLite ownerless coordination or steady SQL.
After the embedded shutdown retry optimizations, startup attribution narrowed
the remaining ordinary process-style startup cost to native storage-engine
startup inside MariaDB plugin initialization: a reduced production sample
reported `start_mysql_server_init_ms_avg=94.364`,
`startup_server_components_plugin_init_ms_avg=52.325`,
`startup_storage_engine_init_innodb_ms_avg=45.038`, and
`startup_innodb_init_srv_start_ms_avg=44.983`.
The follow-up `srv_start()` attribution probe keeps this as native InnoDB
startup work rather than ownerless coordination: a reduced production sample
reported ordinary warm open/close at `162.474 ms`,
`start_mysql_server_init_ms_avg=129.505`,
`startup_innodb_srv_start_total_ms_avg=82.674`,
`startup_innodb_srv_start_log_rebuild_ms_avg=31.326`,
`startup_innodb_srv_start_recovery_bootstrap_ms_avg=25.988`, and
`startup_innodb_srv_start_system_tables_ms_avg=12.881`, while ordinary
active-runtime reconnect stayed cheap at `0.695 ms`. The next performance work
should therefore inspect native redo/log rebuild and recovery bootstrap before
claiming a broader branch regression.
The follow-up log-rebuild attribution split showed that steady no-rebuild
garbage cleanup was not the main cost: a reduced production sample reported
five `srv_log_rebuild_if_needed()` calls, four no-rebuild calls with
`startup_innodb_log_rebuild_no_rebuild_delete_log_files_ms_avg=0.384`, and one
actual rebuild at `startup_innodb_log_rebuild_total_ms_avg=130.486`, dominated
by `startup_innodb_log_rebuild_create_log_file_ms_avg=99.935` and
`startup_innodb_log_rebuild_resize_rename_ms_avg=27.111`. In that sample,
steady warm-open attention shifted to recovery bootstrap and InnoDB system-table
startup, while creation-heavy paths may separately consider first-post-create
redo rebuild avoidance.
The recovery/system-table attribution split then reported ordinary warm
open/close at `118.760 ms`, `start_mysql_server_init_ms_avg=95.526`,
`startup_innodb_srv_start_recovery_bootstrap_ms_avg=25.896`, and
`startup_innodb_srv_start_system_tables_ms_avg=12.288` in a reduced sample where
the redo rebuild bucket was effectively idle. Recovery bootstrap was dominated
by `startup_innodb_recovery_start_ms_avg=21.594` while the system-table bucket
was almost entirely `startup_innodb_system_tables_open_tmp_ms_avg=12.224`.
A later rebuilt-binary five-iteration sample triggered two actual redo rebuilds
and reported `startup_innodb_srv_start_log_rebuild_ms_avg=52.438`,
`startup_innodb_log_rebuild_total_ms_avg=130.372`,
`startup_innodb_recovery_start_ms_avg=19.767`, and
`startup_innodb_system_tables_open_tmp_ms_avg=10.088`. These samples separate
the remaining non-rebuild warm-open cost from redo rebuild trigger frequency:
checkpoint/recovery startup and temporary tablespace opening are the main
non-rebuild native startup costs, while actual redo rebuilds still dominate any
iteration in which they occur. Reason attribution then showed the repeated
actual rebuilds were caused by size mismatch only, with `ib_logfile0` observed
at `100663304` bytes versus the configured `100663296` bytes and matching
`FORMAT_10_8` format.
Embedded clean-shutdown tail truncation now normalizes that physical tail after
MariaDB's clean shutdown LSN/checkpoint checks pass. A reduced production probe
reported zero warm-open redo rebuilds and ordinary warm open/close at
`124.153 ms`; the matching public open/close bench ended with `ib_logfile0` at
exactly `100663296` bytes.
A follow-up recovery-start attribution split the remaining non-rebuild InnoDB
startup bucket. In a reduced production sample, ordinary warm open/close was
`120.698 ms`, `startup_innodb_srv_start_total_ms_avg=38.910`,
`startup_innodb_recovery_start_ms_avg=18.930`,
`startup_innodb_recovery_start_scan_initial_ms_avg=15.194`,
`startup_innodb_recovery_start_scan_rescan_ms_avg=3.724`, and
`startup_innodb_system_tables_open_tmp_ms_avg=10.001`.
`trx_lists_init_at_db_start()` is now visible in compact summaries but was a
smaller child in that sample at `startup_innodb_recovery_trx_lists_ms_avg=2.931`,
dominated by rollback-segment restore over 640 fixed rollback-segment entries
and five cached undo slots with no active or prepared recovered transactions.
The next non-rebuild startup optimization targets are therefore clean redo scan
work and temporary tablespace opening, not ownerless coordination setup.
A follow-up temporary-tablespace attribution split the latter bucket. In a
reduced production sample, ordinary warm open/close reported
`startup_innodb_system_tables_open_tmp_ms_avg=12.255`, exactly matched by
`startup_innodb_temp_tablespace_total_ms_avg=12.255`. The dominant child was
repeated file create/open work
(`startup_innodb_temp_tablespace_open_or_create_ms_avg=10.007`,
`create_new_calls=5`, `reuse_existing_calls=0`), followed by temporary
rollback-segment creation
(`startup_innodb_temp_tablespace_rseg_create_ms_avg=2.153`). Cleanup,
file-spec validation, fil-system open, and header initialization were
sub-millisecond in that sample. Ownerless warm opens showed the same temporary
tablespace shape, while still occasionally paying actual redo rebuild time;
redo rebuild trigger frequency remains a separate performance target from the
temporary tablespace create/open cost.
A follow-up public API branch/main parity benchmark now builds
`tools/mylite_public_open_close_bench` and measures only portable
`mylite_open()` plus `mylite_close()` behavior over an InnoDB table. Against
`origin/main` (`4760d5128096e4560bc62cc19f7066bc15ff07d8`), the current
ownerless branch (`793a085c2c2d25b74610ae1df1f965569bfe4013`) was faster in
two warmed 20-iteration local samples: `141.430-144.442 ms` per warm
open/close versus main at `346.062-347.400 ms`. A matching internal branch
probe reported ordinary warm open/close at `138.025 ms`, with `112.727 ms`
open, `25.295 ms` close, and active-runtime reconnect at `0.891 ms`, so the
current performance focus remains native MariaDB/InnoDB process lifecycle
startup rather than ownerless coordination or active in-process reconnect.
The WordPress PHPUnit CI filters now use exact method-level process-isolated
shards instead of broad mixed-class filters, while the two class-level
`@runTestsInSeparateProcesses` files stay excluded from the non-isolated
bucket. CI-shaped production verification on 2026-06-10 passed the exact
deferred-reconnect shard as 31 tests in `198.815s` shell real, the exact
then-eager-reconnect shard as 22 tests in `121.294s` shell real, and the
non-isolated remaining shard as 28,687 tests in `2757.813s` shell real. This
does not reduce the ordinary WordPress suite volume, but it keeps
process-isolated timing from being inflated by unrelated non-isolated methods
in large mixed classes. The current CI path keeps those exact filters and runs
without child-process profiling by default, but the process-isolated steps keep
a lightweight parent-side child timing summary enabled so CI reports child
count, child runtime, lock-release, reconnect, child-script, and
outer-minus-script averages without enabling full child profiling or mysqli
child aggregation. Full diagnostic child profiling remains available through
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` outside the critical
timing steps. Diagnostic and CI child-script timing both use a per-child temp
file instead of child stderr; a focused production process-isolated emoji
method reported `3.951s` of child script time inside `4.335s` of child-process
runtime, leaving about `0.384s`
of outer process/pipe overhead for that child. Mysqli profile blocks can now
carry a sanitized `mylite_mysqli_profile_context` value; the WordPress PHPUnit
patch tags process-isolated children as `wordpress_phpunit_child` and the
harness emits `mylite_mysqli_profile_child_aggregate_*` rows, separating
child-only MyLite open/close/query cost from parent and setup PHP processes. A
focused production profiled emoji method reported child-only mysqli totals of
`1.685s` open, `1.338s` close, and `1.484s` query work inside `6.050s` of
child script runtime, keeping the next optimization target on child
open/close/query execution rather than parent `proc_open()` overhead. The
UI/filesystem process-isolated CI shard now sets
`MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`, which maps to WordPress'
`WP_TESTS_SKIP_INSTALL=1` only inside PHPUnit children, and the verified
production CI path now also sets
`MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0` for that shard. Focused
emoji coverage dropped child script time to `1.776s`; the full filter first
passed `22` tests in `56.294s` reported time with eager parent reconnect, then
passed the same `22` tests through the renamed reconnect-disabled shard in
`28.749s` reported time and `38.197s` shell real, with parent reconnect at
`0.009 ms` per child and lock release at `13.020 ms` per child. Direct child
skip-install remains unsafe
for factory-heavy deferred tests: on 2026-06-18, `Tests_Admin_ExportWp` and the
two process-isolated `Tests_Sitemaps_Sitemaps` methods both reproduced the
WordPress factory data-shape failure where a `WP_Error` object reaches
`wpdb::prepare()`. The harness therefore adds
`MYLITE_WORDPRESS_PHPUNIT_CHILD_RESTORE_BASELINE=1` for that shard. With the
parent `wpdb` handles closed, PHPUnit restores the prepared MyLite baseline
directory before each child and then runs the child with
`WP_TESTS_SKIP_INSTALL=1`. The exact deferred factory-heavy filter passed `13`
tests in `46.688s` shell real, with `13` baseline restores totaling `2.936861s`
(`225.912 ms` per child), compared with the previous install-required local
sample at `110.170s` shell real. The harness now uses
`cp -a --reflink=auto --` for prepared-baseline creation, parent restore, and
child restore, so copy-on-write filesystems can clone unchanged MyLite database
extents while non-CoW filesystems retain the previous archive-copy behavior. A
2026-06-20 focused production rerun against CI's pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56` passed the same factory-heavy filter
with `13` child processes, `13` child baseline restores, `2.905252s` total child
baseline-restore time, and `223.481 ms` average restore time per child on the
local tmpfs-backed database mount. CI uses that baseline-restored deferred
shard while keeping the already-safe deferred skip-install and UI/filesystem
skip-install shards on reconnect-disabled paths.
The long non-isolated shard now also excludes the whole `Tests_DB*` class
family with a leading `^(?!Tests_DB)` negative lookahead, matching the
dedicated `^Tests_DB` database shard and preventing database-prefix tests such
as `Tests_DB_Charset`, `Tests_DB_dbDelta`, and `Tests_DB_RealEscape` from
being timed twice.
The same production WordPress PHPUnit job keeps harness-owned JUnit timing
disabled by default after completed production branch runs without JUnit
reported the long non-isolated shard at `1156.633s` shell real in an earlier
run, `691.743s` shell real in the post-compressed-BLOB-size-matrix run, and
`661.843s` shell real in a later green run where the whole WordPress job
completed in about `19m45s`. CI first derived three visible non-isolated timing
steps from the same restored `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`:
REST classes, query/theme/block/token classes, and the remaining core classes.
That kept the same test coverage while showing which broad class family owned
the remaining non-isolated wall time. The harness-owned JUnit slowest-class
and slowest-method report remains available through
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` for targeted diagnostics, but it is not
enabled on the critical CI timing path. The CI timing path also sets
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1`, causing the harness to pass
`--no-logging` to PHPUnit when no explicit logging arguments are supplied, so
WordPress' default `phpunit.xml.dist` JUnit logger does not add XML generation
work to the split test-only timings. Diagnostic runs that need JUnit must set
`MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=0` together with
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. Those diagnostic JUnit runs now accept
`MYLITE_WORDPRESS_PHPUNIT_SLOW_REPORT_LIMIT` and emit slow-report aggregate
distribution rows for testcase count, class count, total testcase time,
top-class time, selected top-class time, and selected top-class ratio, with the
compact aggregate rows appended to the WordPress timing summary. A 2026-06-19
production-shaped diagnostic run over the corrected non-isolated remaining
filter passed 16,811 tests with `508.734s` shell real and `500.281s` PHPUnit
reported time; the generated JUnit XML summed `417.660s` across 16,809
testcases and 845 classes, with the top 20 classes accounting for only
`113.720s` (`0.2723`) and the top classes spread across terms, media, user,
post, comment, auth, template, XML-RPC, and customize coverage. That keeps the
critical CI path on no-logging timings and points future wall-clock reduction
toward broader sharding or query-profile slices rather than one slow WordPress
class. The follow-up CI timing-summary slice keeps those same
production/test-only phases but records their key `wordpress_*`,
`wordpress_perf_summary_*`, and any explicitly profiled
`mylite_mysqli_profile_*` metrics in one Markdown file that the final WordPress
CI step publishes to the GitHub step summary.
The next production timing split uses the latest green `6d7afdae` timing
summary, where `phpunit-non-isolated-remaining` was still the largest shard at
`263.471s` shell real versus `162.786s` for REST and `155.892s` for
query/theme/block/token. CI now adds a fourth visible non-isolated shard,
`phpunit-non-isolated-content-user`, for content, user, customize, template,
XML-RPC, option, and metadata class families, and the remaining shard excludes
that family in addition to REST and query/theme/block/token. The production
audit guards the new split and keeps all timing-producing WordPress shards on
the same Release MyLite and MinSizeRel MariaDB embedded artifacts.
After the follow-up content/user split, green run `27896266558` showed
`phpunit-non-isolated-rest-controller` as the new PHPUnit test-only tail at
`147.060s` shell real, ahead of `phpunit-non-isolated-remaining` at
`122.608s`, `phpunit-non-isolated-content-media` at `117.366s`, and
`phpunit-non-isolated-user-auth` at `30.253s`. CI now splits the REST
controller tail into WordPress content-object controllers and other REST
controllers while preserving the same production build guards and REST
non-controller boundary.
After that split, green run `27896697422` reported REST controller fanout at
`71.608s` and `58.761s` shell real while `phpunit-non-isolated-remaining`
became the new tail at `121.495s` and `phpunit-non-isolated-content-media`
remained close at `118.110s`. CI now fans out those two broad tails into
media/comment, content/data, remaining/platform, remaining/admin-site, and
remaining/other shards, preserving the existing DB, isolated, REST,
query/theme, block/token, and user/auth boundaries.
After that fanout, green run `27897052195` reported the new fanout shards at
`72.858s`, `44.906s`, `37.853s`, `26.378s`, and `61.672s` shell real while
`phpunit-non-isolated-query-theme` became the new tail at `105.802s`. CI now
splits query/canonical classes from theme classes while keeping the broad
query/theme regex as the remaining-shard exclusion authority.
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
The page-write stats-off fast-path slice is a diagnostics-only cleanup:
disabled page-write elapsed scopes now return on the existing zero timing
sentinel before rechecking the stats-enabled flag, focused SQL coverage proves
disabled page-write counters stay zero through the native-support WAL proof
workload, and stats-enabled probes still emit page-write summaries. The first
stats-off sample after the cleanup reported ownerless autocommit at
`1046.79 ops/s` versus ordinary autocommit at `3673.54 ops/s`, ratio `0.2850`,
so this does not change the remaining performance target.
The page-publish stats-off fast-path slice applies the same production-path
discipline to ownerless page-publish attribution: the native publish hot path
now snapshots the disabled page-publish stats flag once and skips
diagnostics-only page-type, identity, history-proof, and failure/success
counter helpers when those stats are off. Publication, page-log append,
history-proof success marking, checkpoint ordering, and recovery semantics are
unchanged. Focused SQL coverage proves a stats-disabled ownerless write leaves
representative page-publish counters at zero while the enabled attribution path
still reports native-support and history-proof counters.
The commit-visibility stats-off fast-path slice applies the same rule to the
ownerless transaction commit visibility diagnostics: disabled elapsed scopes now
return on the zero timing sentinel before rechecking the stats flag, and
visible-fast versus conservative-flush reason counters use one enabled-state
snapshot per ownerless commit block. Focused SQL coverage proves a
stats-disabled ownerless write leaves representative commit-visibility counters
at zero while enabled visible-fast attribution remains covered. Commit
publication, log flush, page-visible LSN publication, page flushing, and lock
release ordering are unchanged.
The transaction-image in-place preparation slice removes a temporary
page-sized allocation and copy from transaction-deferred page publication. The
captured page image is private to the committing transaction and is cleared
during transaction cleanup, so the publisher now prepares that buffer with the
same InnoDB checksum helper before handing it to the page-version hook.
Transaction-image counts, fallback publication, page-log format, checkpoint
ordering, and recovery semantics are unchanged.
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
ordinary at `2907.78 ops/s`. A follow-up SYS fill-sparse direct-encode slice
then removed discarded compact sparse materialization for `FIL_PAGE_TYPE_SYS`
pages when the existing fill-sparse record already wins. The reduced 1000-row
stats-enabled production probe preserved `3010` single-row autocommit page-log
append calls, `1000` fill-sparse/SYS records, `1746847` payload bytes, and
`72344` SYS payload bytes while append encode moved from the pre-slice
`63.598 ms` sample to `58.064 ms`. Bulk append encode was `32.789 ms` with
the same `1510` append calls, `250` fill-sparse/SYS records, and `1166401`
payload bytes. That finding led to the index-delta page-log format and bounded
base-refresh slices above. The remaining performance target is therefore
native commit/page-publication cost and broader redo/checkpoint reconciliation,
not startup, SQL dispatch, rollback-segment SYS proof byte volume, or fill-run
compression. A follow-up no-dirty publish-batch slice then made the MTR
no-dirty release-loop publication path use the same page-publish batch contract
as the made-dirty scan path and added direct-versus-session append counters.
The reduced 200-row stats-enabled production probe reported `602` page-log
append calls, only `2` direct append calls, `600` session append calls, `400`
session begin/end calls, append lock time `0.565 ms`, fstat time `0.561 ms`,
and encode time `18.583 ms`. That closes the inconsistent unbatched append
path, but it also confirms the next larger performance targets remain page-log
encoding and native InnoDB commit/write-history work.
A follow-up ownerless page-write leave membership fast path now returns from
the MTR leave hook before ownerless transaction/deferred-release policy
resolution when the current page latch slot was not recorded in the
mini-transaction's ownerless page-write vector. The reduced 500-row production
attribution sample kept page-version and native-support publication counts
stable at `3.008` page versions and `2.004` published native-support pages per
insert, while `page_write_leave_total_ms` moved from `5.554` to `4.911` and
the no-dirty commit-log loop sample moved from `76.541 ms` to `65.279 ms`.
This is a bounded hook overhead reduction; the larger targets remain page-log
encoding, native commit/page-publication, and redo/checkpoint reconciliation.
The release-memo membership precheck then applied the same vector-membership
guard before generic `mtr_t::release()` and `release_unlogged()` call the
ownerless page-write leave helper. Focused visible-fast, history-proof,
native-support, FK-cache, and active-reader selectors passed, and a reduced
500-row four-row-bulk production probe kept page-version, native-support,
page-log append, and commit-visibility counts stable while moving bulk
page-write leave calls from `1485` to `1458`, leave total from `2.101 ms` to
`1.989 ms`, and release time from `1.854 ms` to `1.685 ms`; release-memo,
no-dirty-loop, page-log append, and throughput timings remained noisy across
short final probes. This confirms a small release-path cleanup, not a
replacement for history/native proof volume reduction.
The follow-up MTR page-write diagnostic gate moves ownerless page-write
enter, leave, and publish perf counters behind the existing hooks-active and
startup/recovery exits. That keeps enabled ownerless diagnostics intact while
removing disabled stats-flag loads and timer setup from ordinary or inactive
MTR paths. It is a production-path overhead cleanup, not a change to page
publication volume, latch ordering, or recovery semantics.
The ownerless page-publish buffer-reuse slice then removed per-published-page
scratch allocation/free churn from `mtr_t::ownerless_page_write_publish()` by
retaining one aligned transient page buffer per publishing thread and physical
page size. Page images, checksums, native-support attribution, and
history-proof publication are unchanged; the embedded performance probe now
emits publish-buffer reuse hit/miss counters in both detailed and summary
ownerless autocommit output so branch/main timing comparisons can see whether
the hot path avoids repeated allocation.
The follow-up page-log append attribution slice keeps the page-log record
format unchanged and splits stats-enabled append time into delta-base snapshot,
delta encoding, standalone encoding, payload stats, page-type stats, and
delta-base note-update counters. This is profiling evidence for the next
runtime optimization; it does not change page-version volume or durable WAL
semantics. Reduced 50-row stats-enabled production probes showed the previous
aggregate append total hiding both standalone-encode and delta-base note-update
cost; the exact winner is noisy at that sample size, so the next optimization
should use the split counters over a larger run before changing WAL semantics.
The ownerless delta fast-limit slice used those counters to widen the
non-chained index/undo delta fast threshold from `1024` to `2048` bytes without
changing WAL format, checkpoint rewrite, or replay rules. In the reduced
500-row production attribution sample, page-log append calls stayed at `1506`
and payload bytes stayed effectively unchanged (`556202` versus `556208`),
while fast index deltas increased from `410` to `449`, standalone-size probe
calls dropped from `91` to `51`, size-probe time dropped from `7.229 ms` to
`1.157 ms`, page-log append encode time dropped from `22.498 ms` to
`19.103 ms`, and total append time dropped from `49.012 ms` to `44.932 ms`.
This is a bounded page-log CPU heuristic, not a concurrency-semantics change;
the larger remaining performance targets are still native commit/page
publication, remaining non-fast encoding, and redo/checkpoint reconciliation.
A follow-up delta standalone-estimate slice keeps the same WAL and delta
formats but refreshes the process-local fast-decision standalone-size estimate
after exact fallback has already computed the current standalone size and the
delta append succeeds. In the reduced 500-row production attribution sample,
page-log append calls stayed at `1506`, payload bytes stayed effectively flat
(`556191` versus `556202`), fast index deltas increased from `449` to `468`,
exact index deltas dropped from `32` to `13`, standalone-size probe calls
dropped from `51` to `30`, size-probe time moved from `2.505 ms` in the
pre-slice same-host sample to `1.044 ms`, and exact fallback records reusing a
fast-miss payload dropped from `44` to `23`. This remains volatile cache
training; replay, checkpoint rewrite, and exact fallback correctness are
unchanged.
The follow-up standalone size-probe single-pass slice keeps exact fallback's
same delta acceptance rule but computes compact sparse, varint compact sparse,
fill-sparse, and trailing-size evidence for `FIL_PAGE_INDEX` and
`FIL_PAGE_TYPE_SYS` pages in one size-only page pass. Primitive coverage now
forces exact fallback to reject a retained index delta when the current
fill-sparse standalone encoding is smaller than the compact sparse estimate.
A reduced 500-row production attribution sample preserved page-log append
counts, payload bytes, and index fast/exact delta counts while moving one-row
autocommit standalone-size probe time from `0.631 ms` to `0.307 ms` in the
first post-slice sample; repeated bulk samples preserved the same counters but
remained noisy, so this is not claimed as a material throughput fix.
The follow-up delta exact negative-cache slice records repeated exact fallback
standalone rejections in the process-local delta-base slot. A later append
with the same base slot skips the exact standalone-size probe only when the
new fast decision is also a standalone-size rejection; fast-limit misses still
reach exact fallback because existing coverage proves they can produce accepted
exact deltas. Primitive coverage proves the second repeated rejection skips
the probe and still replays byte-identically from the standalone page image,
and a reduced 120-row production probe reported one skipped exact standalone
probe in both the single-row autocommit and four-row bulk shapes. This is a
small page-log CPU heuristic rather than a concurrency-semantics change.
The follow-up delta exact reuse-probe cache records positive exact fallback
reuse observations in the same process-local delta-base slot. When a later
same-base fast-limit miss has an outstanding positive observation, MyLite can
accept the already-built delta payload without repeating the standalone-size
probe; standalone rejection, build failure, base refresh, invalidation, and
max-delta bounds stay conservative. Primitive coverage proves the first exact
reuse still probes, the next same-base append skips the probe and replays
byte-identically from the delta record, and the following append probes again
after consuming the single positive observation. Final reduced simple and
100-row-bulk stats-enabled production probes still used standalone-size probes
for their representative exact-reuse records, so the current measured CI-shaped
workloads should not claim a throughput win from this cache. This keeps WAL
format, checkpoint rewrite, replay, and concurrency ordering unchanged; the
tradeoff is bounded WAL-size risk if a skipped probe would have selected a
smaller standalone record.
The follow-up delta payload direct-copy slice keeps the same non-chained
index/undo delta WAL bytes but resizes the encoded payload once for all raw
changed-byte runs and copies those runs directly instead of repeatedly
appending them with `std::vector::insert()`. This targets the measured
delta-encode subphase without changing page-version volume, payload bytes,
checkpoint rewrite, or recovery semantics; the larger remaining performance
targets remain native commit/page-publication cost and broader
redo/checkpoint reconciliation.
The follow-up sparse payload direct-copy slice applies the same bounded
materialization cleanup to standalone compact-varint sparse and fill-sparse raw
runs. It keeps the ownerless WAL sparse payload formats and checksums
unchanged, but copies raw run bytes into resized vector tails instead of using
range inserts during standalone encoding. This targets the measured
standalone-encode subphase without changing sparse record selection, payload
bytes, replay, checkpoint rewrite, or SQL behavior.
The follow-up delta eligibility reuse slice keeps the same page-log delta
flags, payload bytes, base-cache lock, and checkpoint/replay rules, but
classifies each appended page image once and reuses that delta flag for both
base snapshot lookup and post-append base-note update. Non-delta-eligible page
classes now skip the base-note helper entirely. This is a first-party CPU
cleanup; it does not reduce page-version publication volume or replace the
remaining history-proof records.
The follow-up single-pass delta page-type slice keeps the same index, undo,
and explicit history rollback-segment delta eligibility rules, but loads the
InnoDB page type once while classifying an appended page image. System-space
index pages still stay standalone, unhinted SYS/TRX_SYS records still stay
standalone, and hinted history rollback-segment pages can still select the
history-rseg delta format. This is a small append CPU cleanup only; WAL bytes,
record flags, checkpoint rewrite, replay, and native history-proof
requirements are unchanged.
The follow-up page-log metadata flag coalescing slice adds a single metadata
flag read helper and uses it while collecting native checkpoint proof records,
so snapshot-boundary, external-lineage, and native-support marker bits no
longer require three positioned page-log header reads for the same record.
Unmarked records still use the legacy payload classifier before treating a
record as a user-page checkpoint proof candidate. This is a checkpoint proof
CPU cleanup only; page-log records, marker bits, payload decoding fallback,
checkpoint proof rules, and replay behavior are unchanged.
The delta note slot-reuse slice then carries the process-local delta-base slot
index found during snapshot lookup into the successful post-append note path.
Delta appends and exact-fallback standalone base refreshes revalidate that
preferred slot under the same cache mutex before falling back to the existing
fingerprint/probe loop. The primitive test covers the standalone-refresh branch
with the internal `delta_base_standalone_slot_reuse_records` diagnostic. This
keeps the same non-chained delta records, payload bytes, refresh boundary,
checkpoint rewrite, and recovery behavior while trimming duplicate volatile
cache work.
The follow-up delta-base buffer reuse slice keeps the same cache keys,
admission policy, page-log records, payload bytes, and checkpoint/replay rules,
but refreshes a standalone base by resizing and copying into the existing
unshared cached page buffer when possible. The primitive standalone-refresh
coverage now asserts the diagnostic `delta_base_page_buffer_reuse_records`
counter, and the production performance probe reports the same counter. This
targets allocation churn in the volatile delta-base note phase only; it does
not reduce ownerless page-version volume or replace the remaining history-proof
publication work.
The four KiB delta fast-path follow-up raises the bounded fast acceptance cap
from `2048` to `4096` bytes while preserving the cached-standalone half-size
rule and exact fallback for larger deltas. Primitive coverage proves an index
delta above `2048` bytes and no larger than `4096` bytes is fast accepted with
no standalone-size probe and reads back byte-identically, while a larger
`4096`+ byte delta still records a fast-limit miss, uses exact fallback, and
replays byte-identically. This remains an in-memory page-log encoding decision;
WAL flags, checkpoint rewrite, replay, and SQL-visible behavior are unchanged.
On the same reduced 500-row four-row-bulk production shape, fast-limit
rejections dropped from `41` to `6`, standalone-size probe calls from `174` to
`144`, standalone-size probe time from `3.074 ms` to `1.838 ms`, page-log
encode time from `6.142 ms` to `4.977 ms`, page-log append total from
`8.411 ms` to `7.183 ms`, and ownerless bulk throughput stayed in the same
range at `4880.07` to `4904.94` rows/s.
A local MTR wrapper fast-path prototype that cached
`ownerless_page_write_uses_transaction_release()` per publish pass and reused
the tracked-page lookup for release was rejected after stress evidence: one
prototype produced ownerless reader monotonicity failures and a DDL stress
InnoDB assertion at `trx0trx.cc:1345`; a narrowed ordered-erase variant still
failed the first ownerless stress case. The code was backed out, rebuilt from
the restored source, and the isolated baseline stress case passed again. Future
native commit/page-publication optimization should not repeat this shortcut
without a stronger proof across transaction-deferred publication and DDL
stress.
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
writer-owned native page evidence; the native-successor shortcut is limited to
writer runtimes, while reader-only consumers still need exact native page proof
or retained WAL and do not refresh external pages just to attempt no-live
reclaim. Writer runtimes still use the normal statement, timer, and close
reclaim paths.
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
concurrent commit evidence. When that global statement byte is held by a peer
writer that is itself waiting on this transaction's shared InnoDB or
transaction-scoped page-write lock, the transaction end can proceed after the
shared registry proves the blocker relationship so native `COMMIT`/full
`ROLLBACK` can release the peer wait instead of timing out behind it.
Read-only transactions that only used locking reads keep the conservative
no-global-refresh state while active, but their transaction end no longer waits
behind a peer writer's global statement gate; native InnoDB row/table locks
remain responsible for the SQL wait.
Ownerless statement-lock acquisition defaults to the existing 60 second
internal wait, but a successful session `SET lock_wait_timeout = N` on that
handle now also bounds the ownerless statement-lock wait to `N` seconds so
tests and applications can fail fast on MyLite's directory-owned statement
gate without changing native InnoDB lock timeout semantics. Contended
directory-owned file-lock waits poll adaptively from 1 ms up to the existing
10 ms cap, improving short ownerless handoffs without changing timeout or
lock-compatibility behavior.
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
The WordPress PHPUnit CI compatibility evidence now runs through a single
production setup job plus parallel test-only shard jobs. Shards verify a
tarred runtime SHA256 manifest and use independent external MyLite database
directories before running the same PHPUnit filters, while the final
`wordpress-phpunit-mysqli-mylite` job merges timing rows and preserves the
published artifact name.
The latest timing fanout keeps the same production WordPress PHPUnit surface
but splits the former `phpunit-non-isolated-content-entity` bucket into
`phpunit-non-isolated-content-post-template` and
`phpunit-non-isolated-content-term-taxonomy`, preserving the broad content
regexes for remaining-shard exclusion and adding no SQL, mysqli, native
storage, recovery, or ownerless behavior change.
The follow-up timing fanout keeps that same production surface while replacing
the former `phpunit-non-isolated-rest-controller-other` bucket with
`phpunit-non-isolated-rest-controller-tests-rest`,
`phpunit-non-isolated-rest-controller-wp-test`, and
`phpunit-non-isolated-rest-controller-wp-rest`, and reducing
`phpunit-non-isolated-remaining-other` by moving AI/connectors, navigation, and
runtime/I/O helper families into their own visible shards. The split is
guarded by the production-build audit and does not change SQL, mysqli, native
storage, recovery, or ownerless behavior.
The next timing fanout keeps the same production surface while replacing the
former `phpunit-non-isolated-block-token` bucket with
`phpunit-non-isolated-block-library` for `Tests_Blocks*` classes and
`phpunit-non-isolated-block-support-template` for block supports, templates,
bindings, token-map, and small `WP_Block*` classes. The broad block/token regex
remains the remaining-shard exclusion authority, and the split changes only CI
test-only filters and audit guards.
The latest top-tail fanout keeps the same production surface while replacing
`phpunit-non-isolated-rest-content-post` with primary REST content and
history-controller shards, `phpunit-non-isolated-block-support-template` with
block supports and block template/binding shards, and
`phpunit-non-isolated-media-comment` with media, comment, and XML-RPC shards.
The broad REST content-post, block support/template, and media/comment regexes
remain union and exclusion authorities; no SQL, mysqli, native storage,
recovery, or ownerless behavior changes.
The follow-up REST top-tail fanout keeps that same production surface while
replacing `phpunit-non-isolated-rest-content-primary` with posts and
pages/attachments/comments shards, and replacing
`phpunit-non-isolated-rest-controller-tests-rest` with font/icon and remaining
`Tests_REST*Controller` shards. The broad REST primary and `Tests_REST`
controller regexes remain union authorities; the split changes only CI
test-only filters and audit guards. Green CI run `27915469258` on `a30256b3a`
passed with those shards, moved the split REST tails below the critical path,
and left fixed per-shard Docker/artifact overhead as the next measured
WordPress timing bottleneck.
The shard runtime-image follow-up keeps the same production WordPress PHPUnit
surface and artifacts while changing the shard container image. Setup still
uses the build-capable WordPress Dockerfile for MariaDB/MyLite artifact
production, but it also seeds a separate runtime-image cache. Shards now load a
test-only runtime Dockerfile under the existing harness tag before running the
unchanged PHPUnit filters. This changes CI timing architecture only; SQL,
mysqli, native storage, recovery, and ownerless behavior are unchanged. The
runtime Dockerfile intentionally omits Composer because setup installs and
packages the WordPress/PHPUnit dependencies before shard fanout.
Green CI run `27916382040` on `70fe4caf3` passed with that runtime-image shape.
The timing rollup reported setup at `70s`, a `12s` runtime Docker cache seed,
total shard Docker setup at `737s`, and estimated WordPress critical path at
`138s`, improving over the previous green run's `76s` setup, `815s` Docker
setup sum, and `146s` estimated critical path.
The follow-up remaining-platform fanout keeps that same production surface
while replacing `phpunit-non-isolated-remaining-platform` with
HTML/interactivity, image, and merged format/dependencies/embed shards. The
broad remaining-platform regex remains the remaining-shard exclusion authority;
the split changes only CI test-only filters and audit guards. The split shard
filters are class-prefix filters so PHPUnit method names cannot pull unrelated
classes into a shard, with `Tests_Embed_*` and `WP_Tests_Image_*` represented
as explicit class families rather than accidental substring matches. Green CI
run `27914755402` on `21e72e017` reported setup action time at `67s`, critical
shard `non-isolated-canonical` at `75s`, estimated WordPress critical path at
`142s`, and corrected remaining-platform shard totals of `62s`, `45s`, and
`44s`, so remaining-platform is no longer the measured critical shard.

Ownerless page-version WAL records can now encode zero-heavy page images by
storing a compact 16-bit sparse nonzero-run list, the original 32-bit sparse
nonzero-run list, or a nonzero prefix plus a record flag, while retaining the
full page size and verifying checksums over the reconstructed full page image.
Primitive coverage verifies compact sparse zero-range, legacy sparse fallback,
tail-prefix, and zero-byte payload readback, append-session offset advancement
by encoded payload size, and checkpoint compaction of encoded retained records.
This reduces WAL byte volume for zero-heavy page images without skipping the
history-proof records or changing page-visible publication semantics.

Ownerless native-reclaim unsafe-hook crash coverage uses threshold-crossing
user-page WAL before pausing at native checkpoint proof. This keeps the
deterministic crash/reclaim evidence aligned with the single-owner foreground
reclaim budget, where small still-single-owner writes may intentionally retain
WAL until timer or close-time cleanup.

`START TRANSACTION WITH CONSISTENT SNAPSHOT` now chooses the live ownerless
page-version read LSN before SQL execution when no ownerless writer or redo
reservation is active, so newer repeatable-read transactions can see commits
that completed after older snapshot pins while those older pins still retain
their original WAL boundary.

## Public API

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Open and close a database directory | 🟡&nbsp;Partial | Implemented for read/write local directory paths with one active database directory per process, a `.mylite/` naming convention, validated format-1 metadata, an advisory directory lock, a native-storage baseline layout under the database directory, lazy ownerless coordination-file creation for ownerless/shared-readonly opens or ordinary opens that must recover retained ownerless state, ownerless startup serialization, final no-live ownerless native DDL file-operation checkpoint evidence, and final no-live ownerless shutdown redo-prefix repair using MariaDB-valid checkpoint-page backups |
| Capability reporting | 🟡&nbsp;Partial | `mylite_capabilities()` reports build/profile-available concurrency modes; the embedded backend currently exposes same-process multi-handle support, ownerless shared read-only support, and ownerless read/write support, while each ownerless open still performs database-directory platform validation before runtime startup; `MYLITE_CAP_SHARED_READONLY` means MariaDB server `@@read_only=ON` plus user-visible read-only SQL through ownerless coordination, not InnoDB `innodb_read_only` startup |
| Read-only opens | 🟡&nbsp;Partial | `MYLITE_OPEN_READONLY \| MYLITE_OPEN_SHARED_READONLY` opens an existing directory through ownerless coordination, starts MariaDB with server `read_only=ON`, observes committed ownerless writer changes, rejects user-visible writes with `MYLITE_READONLY`, and rejects same-process ownerless read/write attachment while the read-only runtime is live; bare `MYLITE_OPEN_READONLY` remains reserved until native storage can enforce read-only engine access |
| Direct SQL execution | 🟡&nbsp;Partial | `mylite_exec()` executes controlled one-shot SQL with textual result callbacks in embedded builds, while `mylite_exec_result()` exposes byte lengths plus display/original field and table metadata for one-shot result rows; `mylite_exec_result_with_metadata()` also emits field metadata for empty result sets so mysqli text-result queries can return zero-row result objects without using the prepared path; one-shot execution drains multi-result `CALL`/procedure output when MariaDB reports additional result sets and skips the redundant `mysql_next_result()` probe for ordinary single-result statements; native-storage coverage verifies MyISAM DDL/DML, row/index operations, and explicit InnoDB transaction/recovery behavior across reopen |
| Prepared statements | 🟡&nbsp;Partial | Reusable MariaDB prepared statements are exposed through `mylite_prepare()`, `mylite_step()`, `mylite_reset()`, and `mylite_finalize()` with 1-based parameter binding; ownerless prepared plain reads recover once from the tested stale InnoDB dictionary-cache `1932` case, while prepared writes, DDL, locking reads, and explicit transactions do not use that retry path |
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
| Primary portable database directory | 🟡&nbsp;Partial | Open/create establishes and validates a MyLite-owned directory with `mylite.meta`, `mylite.lock`, `datadir/`, `tmp/`, and process-local `run/`; fresh ordinary read/write opens do not create `concurrency/`, while ownerless/shared-readonly opens and ordinary opens that find durable ownerless runtime files use `concurrency/` metadata, lock, shared-memory, WAL, and checkpoint anchors; `.mylite/` is recommended but not enforced |
| Ownerless concurrency metadata | 🟡&nbsp;Partial | The directory owns a durable concurrency metadata file with format, MariaDB base, database UUID, concurrency generation, and exclusive-mode state, protected by a byte-range `PERSISTED_CONFIG` lock while it is created or validated; `MYLITE_OPEN_OWNERLESS_RW` uses this path instead of the process-wide `mylite.lock` |
| Ownerless platform proof | 🟡&nbsp;Partial | `MYLITE_OPEN_OWNERLESS_RW` and `MYLITE_OPEN_SHARED_READONLY` probe the prepared database directory for cross-process `MAP_SHARED` visibility, byte-range lock conflict, lock release on process exit, file grow/remap, and wait/wake support before ownerless runtime startup; a successful probe writes `concurrency/mylite-ownerless-platform.meta` with the database-directory device id so later opens skip the full probe until the proof is absent or the directory is on a different filesystem; hook-only coverage forces the probe to fail and verifies ownerless opens reject the directory while ordinary read/write opens remain on the non-ownerless path |
| Ownerless shared-memory file | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.shm` is created and grown under `RECOVERY` then `SHM_RESIZE` byte-range locks, validated through `MAP_SHARED`, starts with a fixed 128-byte MyLite header bound to the database UUID, and contains fixed process-registry, wait-channel, MDL lock-table, transaction-registry, read-view-registry, page-version pin registry, InnoDB lock-registry, redo-visibility, page-version-index, dictionary-generation, page-write lock-registry, and AUTO_INCREMENT high-watermark registry segments; clean opens preserve those segments, while dirty, rebuilding, invalid, no-live-process stale, or incompatible-format volatile state is rebuilt with an incremented recovery generation; volatile process active/live counts are read through `MAP_SHARED` mappings so recovery decisions see peer updates; ownerless SQL opens serialize core `mysql.*` compatibility-table bootstrap through `mylite-concurrency.lock` and the full embedded native startup, connection, dictionary-generation initialization, bounded retry after partial MariaDB embedded startup cleanup and redo-prefix restore, plus final no-live ownerless native DDL file-operation or `ALTER TABLE ... AUTO_INCREMENT` checkpoint evidence and shutdown redo-header repair through separate `mylite-runtime-startup.lock`, creating `concurrency/` before taking that startup lock, clearing process-global ownerless SQL/InnoDB hooks before MariaDB embedded shutdown while preserving native hook context until `mysql_server_end()` completes and keeping the shared-file deletion guard installed until shutdown finishes, and avoiding classic `fcntl()` same-file close release hazards; the 16,384-entry page-version index caches the newest WAL record offset per page, is replayed from `mylite-concurrency.wal` when `.shm` is rebuilt, distinguishes absent index entries from incomplete-index or older-snapshot lookups for diagnostics, keeps authoritative WAL scanning for page reads the index cannot prove, performs direct page-index reads and WAL scans under the existing page-log read guard instead of nested checkpoint read locks, classifies stats-enabled WAL-scan misses by page-key absence versus same-page-not-visible outcomes, returns page-log snapshot-boundary/external-lineage flags to the ownerless InnoDB read hook, uses process-local negative caching only after an authoritative WAL snapshot/scan has proven same-page absence instead of before the first index-miss WAL scan, and extends true no-same-page proofs across unrelated page-index generation changes with a WAL-generation/covered-offset tail cache; the index is reclaimed with live peers only when the page-version pin registry has zero active pins; active pins retain WAL until release; guarded ownerless SQL page-version reads cover direct or prepared `SELECT`/`WITH` statements at a live page-version read LSN while the page-visible LSN remains the durable recovery/checkpoint boundary, with a per-handle monotonic read watermark and a shared read pin opened before clean-page refresh; if no page-visible LSN exists yet, the baseline read pin still marks eligible plain reads as ownerless plain-read statements without exposing an external page boundary; successful direct `mylite_exec()` reads keep the handle pin after returning until a replacement read, non-read/current-read statement, error, close, or dead-owner cleanup releases it, and prepared result cursors keep the handle pin until the result is exhausted, reset, or finalized, so active result pins block single-owner statement/timer checkpoint scheduling while the same embedded runtime may still hold stale clean pages; when an eligible read retains a page-version read LSN, visible-boundary external refresh, clean-page refresh, and file-read overlays reject lower visible-boundary replacements for user data/index/blob pages while still refreshing native undo, system, allocation, and recovery pages; current live reads still accept real current ownerless page images that advance the page or are selected by current commit LSN and reject lower retained user-page boundary images after the same handle has already observed the same page at an equal-or-newer ownerless commit boundary or when the page-log record is flagged as a synthesized snapshot boundary; when a handle first observes a new ownerless process generation or advances its handle pin, clean-page refresh bypasses the single-owner skip because peer commits may already be native-checkpointed and reclaimed from WAL; repeatable-read and serializable transactions publish shared page-version pins for that read LSN on first consistent read, `START TRANSACTION WITH CONSISTENT SNAPSHOT` publishes the pin before SQL execution, and pins release on transaction end, rollback/close, or dead-owner cleanup, while transactions with local writes or locking reads avoid global refresh and clean-page refresh skips locally dirty buffer pages |
| Ownerless recovery anchors | 🟡&nbsp;Partial | `concurrency/mylite-concurrency.wal` and `concurrency/mylite-concurrency.ckpt` are created under `RECOVERY` with fixed headers bound to the database UUID; guarded ownerless SQL writes page-version records after the `.wal` header, persists latest raw redo and page-visible LSNs in `.ckpt`, treats visible page-version WAL records as the no-live-process recovery authority, rewinds existing native InnoDB tablespace pages selected by commit LSN first and page LSN second, preserves matching native disk pages in product no-live replay when the native and retained WAL images have the same page LSN, retains complete committed page-version WAL records after no-live-process tablespace replay until native redo/checkpoint reconciliation can prove truncation safe, skips retained page-version records for tablespaces that no longer exist during product no-live replay, replays retained page-version WAL records into the shared page-version index when `.shm` is rebuilt, and seeds rebuilt or clean runtime-attached redo-visibility state monotonically from `.ckpt`; no-live `.shm` rebuilds checkpoint retained reader-boundary WAL without replaying stale page images when their remaining state is stale read-view/page-pin evidence without native writer recovery evidence, and focused SQL coverage verifies single-table dropped, same-schema and cross-schema same-statement multi-dropped file-per-table absence, ordinary-created file-per-table final state with secondary-index metadata/use, LIKE-copy, and CTAS-created file-per-table final states, same-name recreated and `CREATE OR REPLACE TABLE` replacement file-per-table final states, including `CREATE OR REPLACE TABLE ... LIKE` copied-shape replacement and `CREATE OR REPLACE TABLE ... AS SELECT` populated replacement, with page-0 space-id identity checks, cross-schema renamed file-per-table final state, truncated file-per-table post-truncate state, copy-style force-rebuilt file-per-table final state, same-schema and cross-schema multi-pair rename-swap final states, and multi-table dropped-schema absence through ownerless/native reopen before and after forced `.shm` rebuild; non-read-only close now advances local native LSN state when needed, and no-live close first publishes native `FILE_CHECKPOINT` evidence for completed DDL file-operation redo when it is the final live ownerless process, drains a real SQL `ALTER TABLE ... AUTO_INCREMENT` native file-op checkpoint marker after the final live peer closes, drains stale native file-op checkpoint markers even when no page-visible LSN exists, advances page-visible state to a newer raw latest LSN by publishing eligible native support/allocation/system buffer-pool pages and flushing native dirty pages, then refreshes external clean page state, forces a native InnoDB checkpoint, compacts page-version WAL records at or below the durable page-visible LSN covered by that native checkpoint, retains newer complete records, and replaces the shared page-version index before releasing checkpoint locks or leaves WAL scanning enabled as the safe fallback; this close-time reclamation path compacts user-page WAL with no live peers; with live peers, it compacts only when the proof scan has no process-local native page records after a nonblocking ownerless statement gate proves zero active page-version snapshot pins plus no active ownerless native write/recovery state; native-support proof and user data/index records remain retained until no-live close; active pins retain WAL until release, while page-version publish can still synthesize boundary records from a native page whose page LSN is at or below the oldest active pin so active readers and later post-release cleanup have a compact boundary image; bounded repeated same-row and distinct large-row expanding-page SQL writer pressure under a live reader are covered, an opt-in `mylite_open_config.ownerless_page_log_limit_bytes` soft cap returns `MYLITE_BUSY` for direct or prepared ownerless writes when active snapshot pins retain WAL at or above the configured byte limit, and `mylite_ownerless_pressure_status()` reports the active pin count, oldest pin LSN, raw WAL bytes, configured limit, and current throttle-reached state; thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling reuses the native reclaim path for user-page WAL when no peer process is live, and live-idle coverage retains native-support proof WAL while peers remain live and drains it after the final live peer closes, while user WAL remains covered by active-pin/live-writer retention tests, and the ownerless runtime scheduler can independently checkpoint retained WAL after a shared read-only snapshot pin releases while the writer remains open and idle; ordinary exclusive read/write opens enable page-version reads only when retained page-version WAL payload records exist, use a checkpoint-visible native clean-page refresh when `.ckpt` still proves a newer ownerless boundary after WAL compaction, and arm ownerless uncheckpointed file-operation recovery only when retained WAL, a native file-op checkpoint marker, or a valid saved redo-header backup proves prior ownerless redo/checkpoint suppression; that recovery path normalizes datadir-prefixed `.ibd` FILE redo names, including the observed leading-separator-stripped datadir form, before prepending the active datadir, so covered ownerless multi-writer commits and DDL policy handoffs remain visible through native exclusive reopen before and after forced `.shm` rebuild without taxing fresh ordinary opens; unsafe-hook SQL coverage now kills a writer after volatile `.shm` page-visible publication but before `.ckpt` persistence, kills after native checkpoint proof but before close-time page-log reclamation, and pauses after native checkpoint proof while a peer commits newer page-version records, proving normal reopen and forced `.shm` recreation do not depend on volatile `.shm` as the only copy of committed updates and that a resumed paused closer does not grow or invalidate already reclaimed WAL; page replay currently resolves existing tablespaces by InnoDB page-0 space id and still relies on the conservative native-file bridge for broader DDL/file lifecycle edge cases |
| Ownerless rename-create stale-reader replay | 🟡&nbsp;Partial | Focused SQL coverage updates an InnoDB file-per-table table while an older ownerless repeatable-read snapshot retains page-version WAL, renames that original table away, creates a different table at the original SQL name, writes both final tables, keeps local post-DDL `INSERT ... VALUES` on the conservative native-page bridge while the external pin is active, and verifies through ownerless/native reopen before and after forced `.shm` rebuild that the moved table keeps the original `SPACE` and the new original-name table has a distinct `SPACE`, with `.ibd` page-0 identity checks for both; broader DDL/file-lifecycle recovery remains partial |
| Ownerless CTAS post-create DML | 🟡&nbsp;Partial | Focused SQL coverage creates a CTAS file-per-table destination while another ownerless process pins a stale repeatable-read snapshot, verifies immediate numeric and payload `UPDATE`, `DELETE`, and `INSERT ... SELECT` statements against that CTAS table, retains page-version WAL while the reader pin is live, and verifies ownerless/native reopen before and after forced `.shm` rebuild; active-reader pressure coverage also blocks post-create `UPDATE`, `DELETE`, and `INSERT ... SELECT` against an existing CTAS destination while retained WAL is at the configured soft limit, verifies no side effects, then verifies CTAS DML succeeds after the pin releases; `tools/ownerless-ctas-dml-trace` emits deterministic CTAS create/update/delete/insert SQL plus repeatable snapshot reader and final oracles for external harness input, with focused Docker-backed MariaDB 11.8 scale-2 replay evidence; exhaustive CTAS DML/crash matrices and randomized external stress remain planned |
| Ownerless consistent-snapshot baseline | 🟡&nbsp;Partial | `START TRANSACTION WITH CONSISTENT SNAPSHOT` on an ownerless read/write handle publishes its page-version pin before SQL execution; when `.ckpt` has no page-visible LSN and `mylite-concurrency.wal` has no payload records, the pin path can seed `.ckpt` and shared redo-visible state from the current native InnoDB checkpoint LSN, giving active-reader boundary synthesis a nonzero baseline without installing ownerless hooks for fresh ordinary opens or doing broad startup-time checkpoint seeding |
| Ownerless page-version log primitive | 🟡&nbsp;Partial | A first-party fixed-record page-version log primitive can initialize a log, initialize at a payload offset, serialize appends with a byte-range lock, store real InnoDB `space_id`/`page_no` pairs including zero, snapshot a stable log end, read a record by WAL offset, reject direct-offset reads whose record identity does not match the expected page, replay committed record metadata for shared-index rebuild, read the latest version visible at a commit LSN without holding the append lock while scanning, compact records at or below a safe commit LSN while retaining newer records, preserve active snapshot data-page boundaries in primitive checkpoint evidence, retain newer snapshot-page records for the general multi-pin path, compact checkpointed post-snapshot records in the single-pin primitive path, require boundaries for R-tree/index/blob/instant/unknown/unrecognized page types while allowing only explicit undo/allocation/tablespace-header/extent/transaction-system/change-buffer/system support pages to bypass the oldest-snapshot check and be dropped after native checkpoint proof, run a checkpoint-completion callback before releasing checkpoint locks, safely truncate the log when every complete record is at or below a safe commit LSN, report missing pages and undersized buffers, durably sync the log before page-visible publication unless the current WAL size and header generation exactly match a process-local already-synced anchor, write embedded-build payload checksums with a MariaDB CRC32C-derived 64-bit value while accepting retained legacy FNV64 checksum records, validate non-delta page-log scan/replay/checkpoint payloads through a streaming full-page checksum for full, trailing-zero, sparse-zero, compact sparse, varint compact sparse, and fill-sparse encodings, and tolerate incomplete or checksum-corrupt tail records; primitive tests cover same-process and cross-process append/read behavior, snapshot-bounded reads, corrupt-tail recovery, corrupt sparse interior rejection and sparse tail tolerance, checkpoint compaction with retained offsets, checkpoint-time page-index replacement, same-version stale page-index publish rejection, checkpoint write-lock blocking behind a cross-process long reader, stale-index WAL-scan recovery, stale-index identity rejection, safe all-record checkpoints, page-index clearing, page-index replacement after WAL-scan fallback, replaying record offsets into a page index, initialized clean sync elision by exact size/generation anchor with append-forced resync, active-pin multi-pin retention versus single-snapshot compaction, native support-page checkpoint drop, R-tree missing-boundary rejection, and applying visible page records to existing native tablespace files, including reading native pages at or before a target snapshot LSN, rewinding a higher-LSN disk page, rewriting a same-LSN different-image disk page, preserving a native same-LSN page in product replay mode, selecting same-page replay winners by the page-log latest-visible commit ordering, strict missing-tablespace rejection, and product-mode dropped-tablespace skipping; guarded SQL tests verify dirty page images are appended to `mylite-concurrency.wal`, indexed in `mylite-concurrency.shm`, replayed into a rebuilt `.shm` page index, reclaimed on no-peer close after native checkpoint evidence, native-support proof WAL is retained while an idle live peer remains open with no page-version pins and reclaimed after that peer closes, user page-version records are retained by live writer, live snapshot pin, and pressure coverage, live repeatable-read snapshot pins block prefix compaction until boundary proof exists, synthesize native page boundaries while a live repeatable-read snapshot pin is active, retain multi-pin newer-record coverage through the active-pin hook path, use hook-backed single-active-pin primitive reclaim to drop checkpointed post-snapshot records when boundary proof is complete while product close-time reclaim retains WAL until active pins release, retained after a killed pinned reader is cleaned while another ownerless peer remains live, then reclaimed after that peer exits, partially compacted when a peer commits newer records while an older closer is paused at native checkpoint proof, and recover after a process is killed at the deterministic pre-truncate recovery-checkpoint fault, after volatile page-visible publication but before `.ckpt` persistence, after native checkpoint proof but before close-time page-log reclamation, or killed with uncommitted same-page updates; ownerless SQL and ordinary exclusive read/write reopens can read page versions for direct and prepared `SELECT`/`WITH` statements at a live page-version read LSN, including transactions with local writes whose own uncommitted redo can hold back the durable page-visible LSN; locally dirty buffer pages remain resident during clean-page refresh; non-forced page-version write refresh accepts strictly newer page-version images but does not overwrite a same-LSN or newer clean local page with an older page-version image; after startup, InnoDB read completion validates ownerless page identity/checksum in a temporary buffer, including `space_id=0` system-tablespace pages, and overlays the disk frame only when the disk frame is invalid for the expected page or older by page LSN; broader DML/DDL and reconstruction of missing DDL-created tablespaces remain on the conservative native-file bridge; the page-visible LSN advances after transaction-owned dirty page images for the commit have been published and the page-version WAL is durably synced or proven unchanged from that process-local synced anchor, with MTR-proven autocommit writes and transaction-deferred dirty pages proven by transaction-page publication allowed to skip the native dirty-page flush while live peer explicit-column-list AUTO_INCREMENT insert targets, DDL, unproved transaction-deferred dirty pages, rollback/deadlock cleanup, and any MTR publish skip/failure keep the conservative flush bridge; page-visible publication is blocked while another live ownerless process is inside an explicit transaction or the shared transaction registry has active read-write transactions, rollback no longer publishes a post-cleanup global `log_get_lsn()` boundary, and global dirty-page scans publish only native support/allocation/system pages instead of user data/index pages; hook tests verify the page-version visibility LSN is scoped to the executing SQL thread |
| Ownerless peer-DDL insert bridge | 🟡&nbsp;Partial | When an ownerless handle observes a newer peer dictionary generation, the next eligible `INSERT ... VALUES` bypasses the ownerless visible fast path and uses the conservative dirty-page flush bridge before fast-path inserts can resume; focused AUTO_INCREMENT column DDL coverage verifies an already-open peer inserts after a rebuild-style `ADD COLUMN ... AUTO_INCREMENT PRIMARY KEY` and later primary-index lookups remain visible through ownerless/native reopen and forced `.shm` rebuild |
| Ownerless DDL allocation refresh guard | 🟡&nbsp;Partial | Ownerless dictionary DDL statements run after pre-statement external page refresh under the dictionary statement write lock; while that lock excludes peer writes, InnoDB suppresses repeated internal external allocation-page refreshes so online DDL does not rewind freshly local file-per-table allocation pages before adding or rebuilding indexes; concurrent ownerless DDL allocation coverage verifies distinct table IDs, spaces, and final secondary-index metadata |
| Ownerless coordination primitives | 🟡&nbsp;Partial | POSIX file-backed `MAP_SHARED` visibility, grow/remap behavior, byte-range lock conflicts, lock release on process exit, internal mapped latch wait/wake plus timeout behavior, fixed-width owner-generation-aware shared latch words, internal cross-process process-slot allocation, heartbeat update, live-slot counting, stale-slot cleanup including exited/zombie-process cleanup, dead-owner lock cleanup, an internal cross-process metadata lock-table primitive with repeated-owner reference counts, same-owner mode upgrades, MariaDB-style granted compatibility for schema IX/S/X and table S/SH/SR/SW/SU/SRO/SNW/SNRW/X modes, stable ownerless MDL schema/table key hashing, an internal cross-process transaction registry primitive for monotonic transaction IDs, active-ID snapshots, oldest-active tracking, stale end rejection, and owner-scoped active-count checks, an internal read-view registry for purge-visible read-view publication and cleanup, an internal page-version pin registry for cross-process snapshot read-LSN publication, oldest-LSN snapshotting, owner cleanup, and slot exhaustion, an internal InnoDB table/record lock-registry primitive with MariaDB-compatible table/gap/insert-intention conflict coverage, conservative same-page physical-X resource conflict coverage for separate process-local buffer pools, shared wait-edge publication, wait cleanup, cross-process wait-cycle detection, final timeout availability rechecks, wait-only missed-wakeup coverage, and table-lock waiter-death owner-cleanup coverage to avoid stale wait entries after missed deadline wakes or killed waiters, a separate internal page-write lock registry for X/SX page-latch write ownership that must not be starved by row-lock-heavy transactions, an internal AUTO_INCREMENT registry that preserves per-table next-value high watermarks across ownerless peers, and an internal redo-state primitive for owner-generation-aware redo latch ownership, nested local entry, latch-free latest-LSN observation, raw latest LSN publication, page-visible LSN publication, monotonic checkpoint seeding, serialized append-range reservation, active reservation and completed-range counts, coalesced out-of-order completed redo ranges, contiguous written-LSN tracking, and dead-owner cleanup are covered as platform evidence; unsafe-hook SQL coverage now kills a writer after shared transaction registration but before the update proceeds, proves a later writer cannot commit past an earlier unwritten redo reservation, and kills a writer after a completed redo write but before latest-LSN checkpoint publication, proving live-peer cleanup remains busy while no-live reopen rebuilds without applying interrupted updates; product opens now allocate and release a directory process slot, validate the transaction, read-view, page-version pin, InnoDB lock, page-write lock, AUTO_INCREMENT, and redo-visibility segments, preserve dead-owner recovery-sensitive state while live peers remain, return busy instead of deleting that state, and rebuild stale volatile coordination after no live owners remain |
| Ownerless MDL hook surface | 🟡&nbsp;Partial | MariaDB's embedded MDL ticket lifecycle has a MyLite hook point for schema/table metadata-lock acquire and release, including cloned tickets, upgrades, downgrades, and release balancing; `libmylite` registers it against the directory-backed MDL lock-table segment using the runtime process-slot owner for ownerless opens, maps schema/table tickets to mode-aware granted-lock compatibility, and covers cross-process `ALTER TABLE` timeout behavior behind an active transaction |
| Ownerless InnoDB transaction and read-view hook surface | 🟡&nbsp;Partial | InnoDB maximum transaction ID reads, transaction ID allocation, read-write transaction registration, transaction serialisation-number assignment, active transaction snapshots, deregistration, read-view publication/removal, and purge oldest-view snapshotting have guarded MyLite hook surfaces covered by embedded InnoDB SQL tests; active transaction snapshot reads retry transient ownerless hook errors before retaining the persistent-error abort path, with embedded hook coverage for an injected transient snapshot error; internal or recovered transactions that were never registered in the ownerless shared registry still receive serialisation numbers from the shared monotonic sequence, and missing deregistration is treated as a no-op; ownerless opens install those hooks against directory-backed shared state |
| Ownerless InnoDB lock hook surface | 🟡&nbsp;Partial | InnoDB table-lock creation/removal, record-lock bitmap bit set/reset, waiting-lock grant, record-lock object dequeue, wait enqueue/reset, discard paths, AUTO_INCREMENT counter reservation, and X/SX data-page latch write ownership have guarded MyLite hook coverage that mirrors granted native locks and local wait edges into the directory-backed InnoDB lock-registry segment, while B-tree/external-value page writes use a dedicated page-write lock-registry segment; locks acquired before `trx_t::id` exists use a stable transient MyLite lock identity until release, and explicit-transaction page-write locks acquired under a transient page-write identity remain transaction-scoped until commit or rollback; embedded SQL tests verify lock entries appear during real InnoDB write transactions and DDL locking-read paths, same-owner conflicts remain native InnoDB's responsibility while local row-lock waits publish and clear shared wait entries, ordinary `REC_NOT_GAP` row locks keep record-level identity, physical same-page X resources serialize only when the native lock has no record/gap flags, insert-intention checks honor peer gap/next-key locks, ownerless simple inserts reserve AUTO_INCREMENT values through a shared table-ID-keyed high-watermark registry, and row-lock-heavy transactions do not starve page-write serialization; granted locks release on commit after dirty pages are flushed through the transaction commit LSN, rollback, and normal close; dead-owner cleanup no longer removes granted lock or redo-visibility state while live peers remain because that state is transaction-recovery evidence. Pre-grant reservation prevents a local grant when the shared registry already contains a conflicting external record lock, synthetic same-page cross-process X resources serialize to avoid process-local buffer-pool page-image overwrites, queued same-page waiters cannot be bypassed by new arrivals, dirty page-write ownership is acquired only when a persistent page becomes dirty, deferred dirty page-write deadlocks and rollback-segment history commit page-write deadlock reports retry instead of proceeding unlocked or asserting, mini-transaction-local page-write acquisitions are released even when no modify memo remains, explicit transaction handler write locks use statement-scoped tablespace gates as statements open tables and release those gate markers at statement end while preserving real dirty page-write locks until commit or rollback, a cross-process external record conflict waits and wakes after release, undo segment creation holds ownerless tablespace-allocation serialization through its mini-transaction, undo/system page versions publish at mini-transaction scope after ownerless redo is written while user data/index pages remain transaction-visible, later undo/system writes in explicit transactions still acquire page-kind-aware ownerless pre-write ownership after user data pages have been deferred, autocommit ownerless statements refresh local InnoDB redo/page state and durable tablespace header/allocation metadata between statements, with a single-owner epoch proof skipping non-forced page-write, space-metadata, and explicit-transaction buffer-pool first-write refresh only after a checkpoint baseline exists and while no peer process, peer-owned page-version pins, or peer-history invalidation are present, while explicit transactions avoid global refresh and peer/live cases keep conservative buffer-pool refresh, ownerless read paths advance the local durable LSN for externally flushed pages, rollback-segment history commits refresh the relevant tablespace header and current first history-list undo page before validating free-list bounds and splicing file-list links, ownerless embedded waits honor the current SQL session lock-wait timeout when the InnoDB transaction lacks `trx->mysql_thd`, cross-process deadlocks return MariaDB errno 1213, shared-registry timeout maps to MariaDB errno 1205 after a final availability recheck, and post-wait refresh targets the waited record page instead of globally evicting pages from an active writer transaction |
| MariaDB metadata files | 🟡&nbsp;Partial | Controlled schema and MyISAM table metadata lifecycle is covered for `db.opt`, `.frm`, create, alter, rename, and drop paths inside `datadir/` |
| InnoDB files | 🟡&nbsp;Partial | Representative InnoDB tablespace, redo, undo, and temporary files are configured and covered inside the MyLite database directory; ownerless read/write opens use a private InnoDB temporary tablespace under each process runtime `tmp/` directory so same-named temporary tables remain connection-local across peers; production startup attribution now distinguishes no-rebuild redo cleanup, actual redo rebuilds, size/format rebuild reasons, physical `ib_logfile0` size at rebuild time, and non-rebuild recovery-start/transaction-list subphases; the latest attribution sample showed actual rebuilds caused by a physical 8-byte redo size tail (`100663304` observed versus `100663296` configured) with matching format and zero internal/physical divergence, embedded clean-shutdown tail truncation now normalizes that tail only after MariaDB's clean shutdown LSN/checkpoint checks pass while leaving the startup rebuild predicate intact, and final no-live ownerless closes temporarily use MariaDB clean shutdown plus the existing saved redo-header repair path when retained ownerless WAL contains no page-version payload records, covering both ownerless-created write-close and ordinary-created metadata-only ownerless attach paths while live peers and retained payload records keep the crash-style shutdown policy; ownerless read/write mode rejects explicit `ALTER TABLE ... DISCARD/IMPORT TABLESPACE` plus create/alter `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options, including representative idempotent, replacement, and temporary create-table spellings, before unproven native file-layout or detach/import paths; buffer-pool dump/load is disabled at startup and matching dynamic InnoDB buffer-pool dump/load variable assignments are rejected because the advisory `ib_buffer_pool` file is unsafe under concurrent embedded processes and is not needed for durability |
| MyISAM files | 🟡&nbsp;Partial | Controlled lifecycle and native table operation coverage verifies `.MYD` and `.MYI` table files stay inside `datadir/` across create, row DML, copy alter, rename, drop, and reopen |
| Aria files | 🟡&nbsp;Partial | Runtime startup sets `--aria-log-dir-path=<db>/datadir`; explicit Aria table coverage verifies `.MAI` and `.MAD` files under `datadir/` |
| MEMORY definitions | 🟡&nbsp;Partial | Explicit MEMORY table coverage verifies persistent table metadata under `datadir/` and empty row state after reopen |
| MyLite-owned transient paths | 🟡&nbsp;Partial | Durable database paths use per-runtime `tmp/<runtime-id>/`, `run/<runtime-id>/`, and `mylite.lock` inside the database directory; clean close removes the current runtime's children and prunes an empty `run/` root, clean exclusive open replaces stale inactive runtime children after taking the directory lock, and `:memory:` uses a transient runtime directory that is removed on final close |
| Durable files outside the database directory | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-surface policy coverage rejects or disables known server-owned paths that could create replication, binlog, performance-schema, or `mysql.*` sidecars outside the supported application-storage model, and ownerless active-reader pressure coverage verifies representative process-control, account/grant, plugin, binlog, logging, query-cache, event/scheduler, and host-file export/import statements keep the explicit server-surface policy error instead of becoming retryable pressure-limit failures; table DDL rejects `DATA DIRECTORY` and `INDEX DIRECTORY` options before native engines can route table files to caller-named locations outside the MyLite directory, including ownerless coverage for representative create, alter, and partition-level directory-option spellings |

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
Clean embedded shutdown also truncates a physical `ib_logfile0` tail beyond
the configured logical redo size only after MariaDB's shutdown LSN/checkpoint
checks have succeeded, avoiding repeated size-mismatch rebuilds on clean
reopen without changing crash recovery policy.

## SQL Surface

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE` | 🟡&nbsp;Partial | Controlled MyISAM create, drop, and rename lifecycle is covered for native metadata and engine files; explicit InnoDB, Aria, MEMORY, MariaDB default-engine create, `CREATE TABLE ... LIKE`, and `CREATE TABLE ... SELECT` coverage is also present, including ownerless peer-refresh coverage for idempotent `CREATE TABLE IF NOT EXISTS` / `DROP TABLE IF EXISTS`, `CREATE OR REPLACE TABLE` replacement of an existing InnoDB table definition and rows, `LIKE`, CTAS, ordinary inline secondary `INDEX` creation with duplicate inline key-name failure and no leaked table, same-schema parent/child foreign-key `RENAME TABLE` metadata/enforcement, cross-schema parent/child foreign-key `RENAME TABLE` metadata/enforcement, same-schema and cross-schema multi-pair parent/child foreign-key `RENAME TABLE` metadata/enforcement, cross-schema InnoDB `RENAME TABLE` with `.frm`/`.ibd` movement between schema directories, and a multi-pair InnoDB rename cycle that swaps tablespace identities; `CREATE TABLE ... DATA DIRECTORY` and `CREATE TABLE ... INDEX DIRECTORY` are rejected to preserve single-directory storage, ownerless policy coverage verifies create-time table-directory option rejection before external paths are created, and ownerless read/write mode rejects partitioned `CREATE TABLE` plus create-time `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options, including idempotent, replacement, and temporary create-table spellings, before entering unproven partition metadata, storage-option, and file-lifecycle paths |
| `ALTER TABLE` | 🟡&nbsp;Partial | Controlled MyISAM `ADD COLUMN` and copy-style `ADD KEY` lifecycle is covered across close and reopen; representative default-engine InnoDB column modify/change and index add/drop changes are covered, ownerless peer-refresh coverage exercises an online/in-place index alter, explicit online DDL option variants (`ALGORITHM=INSTANT, LOCK=DEFAULT` column add/drop plus stored-column placement and column rename, `ALGORITHM=INSTANT` placed stored-column ADD with `LOCK=SHARED` and `LOCK=EXCLUSIVE`, instant virtual generated-column ADD with `LOCK=SHARED` and DROP with `LOCK=EXCLUSIVE`, `ALGORITHM=NOCOPY, LOCK=NONE` secondary-index create and drop, `ALGORITHM=NOCOPY, LOCK=DEFAULT` secondary-index create and drop, `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE` secondary-index create and drop, `ALGORITHM=INPLACE, LOCK=NONE` secondary-index create, `ALGORITHM=INPLACE, LOCK=SHARED` secondary-index create, unique secondary-index create and drop with `ALGORITHM=INPLACE, LOCK=SHARED`, `ALGORITHM=INPLACE, LOCK=DEFAULT` secondary-index create and drop, `ALGORITHM=COPY, LOCK=EXCLUSIVE` column/rebuild paths, and explicit no-lock index ignored/not-ignored toggles), secondary-index rename, index ignored/not-ignored toggles, primary-key replacement with hook-build crash recovery for plain and composite direction replacements plus idempotent ADD PRIMARY KEY no-op preservation and crash recovery, descending-key metadata, composite direction metadata, AUTO_INCREMENT-column preservation, and AUTO_INCREMENT descending-key metadata, foreign-key add/drop, generated-column add/drop and same-kind expression replacement, table charset conversion, row-format rebuild including focused compressed `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`, `KEY_BLOCK_SIZE=4`, and `KEY_BLOCK_SIZE=16` rebuilds with peer metadata refresh and native ZBLOB page evidence, table comment metadata, `ALTER TABLE ... FORCE` rebuild, column default set/drop, column add/modify/rename/drop ALTERs, idempotent `ADD COLUMN IF NOT EXISTS` / `DROP COLUMN IF EXISTS` behavior with duplicate-add errno 1060 plus hook-build crash recovery for duplicate-add and missing-drop no-op branches, hook-build crash recovery for missing `MODIFY COLUMN IF EXISTS`, `RENAME COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`, `ALTER COLUMN IF EXISTS SET DEFAULT`, and `ALTER COLUMN IF EXISTS DROP DEFAULT` no-op branches including missing rename/change/default preservation on generated-column/CHECK expression tables, explicit InnoDB instant ADD/DROP/reorder column metadata from another process, and explicit instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants from another process, and concurrent ownerless DDL allocation coverage exercises online index add/drop replacement; `ALTER TABLE ... DATA DIRECTORY` and `ALTER TABLE ... INDEX DIRECTORY` are rejected to preserve single-directory storage with ownerless policy coverage for representative alter-time directory-option spellings, while ownerless read/write mode rejects partitioning plus add/drop/rebuild/optimize/analyze/check/repair/coalesce/truncate/reorganize/exchange/convert/remove partition-maintenance ALTERs before entering unproven partition metadata and file-lifecycle paths, rejects `DISCARD/IMPORT TABLESPACE` before unproven native file detach/import paths, and rejects alter-time `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, `ENCRYPTION_KEY_ID`, and table `TABLESPACE` options before unproven storage-option file-layout paths; broader edge cases remain planned |
| Standalone `CREATE INDEX` / `DROP INDEX` | 🟡&nbsp;Partial | Representative default-engine InnoDB standalone index create/drop is covered through MariaDB DDL and native engine metadata; ownerless coverage verifies already-open peer refresh for standalone InnoDB secondary-index create/use/drop, idempotent secondary-index create/drop with duplicate-create errno 1061, hook-build crash recovery for duplicate top-level `CREATE INDEX IF NOT EXISTS` preserving the original key part, missing top-level `DROP INDEX IF EXISTS` preserving the real index, duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` preserving the original key part, and missing `ALTER TABLE ... DROP INDEX IF EXISTS` preserving the real index, `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and `ALTER TABLE ... DROP INDEX IF EXISTS` table-element idempotency, `CREATE OR REPLACE INDEX` replacement of an existing index name over a different key part, multi-column unique-index create/enforce, idempotent `CREATE UNIQUE INDEX IF NOT EXISTS` no-op preservation with duplicate plain-create errno 1061 plus hook-build crash recovery for duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` preserving the original unique key, `CREATE OR REPLACE UNIQUE INDEX` enforcement movement to a replacement key definition, `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` create/no-op preservation with duplicate plain-add errno 1061 plus hook-build crash recovery for duplicate ALTER unique add preserving the original unique key, missing/repeated `ALTER TABLE ... DROP INDEX IF EXISTS`, and drop semantics, unique descending secondary-index metadata/enforcement/drop semantics, unique prefix secondary-index metadata/enforcement/drop semantics, unique prefix-plus-direction secondary-index metadata/enforcement/drop semantics, utf8mb4 prefix secondary-index character-count metadata/enforcement/drop semantics, unique TEXT/BLOB prefix secondary-index metadata/enforcement/drop semantics, unique TEXT/BLOB prefix-plus-direction secondary-index metadata/enforcement/drop semantics, descending secondary-index `COLLATION = 'D'` metadata refresh/use/drop, mixed ASC/DESC composite-index `COLLATION` metadata refresh/use/drop, prefix-plus-direction secondary-index `SUB_PART`/`COLLATION` metadata refresh/use/drop, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix secondary-index `SUB_PART` metadata refresh/use/drop, TEXT/BLOB prefix-plus-direction secondary-index `SUB_PART`/`COLLATION` metadata refresh/use/drop, and final index absence through ownerless/native reopen before and after forced `.shm` rebuild; ownerless read/write mode rejects top-level, ALTER, inline, and idempotent `FULLTEXT` and `SPATIAL` index DDL before entering unproven special-index native storage paths |
| Generated-column secondary-index DDL | 🟡&nbsp;Partial | Ownerless coverage verifies standalone `CREATE INDEX`/`DROP INDEX` over deterministic stored and virtual generated InnoDB columns, unique generated-column indexes, prefix generated-column indexes with `SUB_PART` metadata, mixed-direction composite generated-column indexes, accepted explicit `NOCOPY`/`INPLACE` generated-column index add/drop options, already-open peer `INFORMATION_SCHEMA.STATISTICS` refresh, forced-index reads while present, generated-value recalculation after peer DML changes base columns, indexed stored and virtual generated-column expression replacement with forced-index reads over recalculated replacement values, forced-index failure after drop, and ownerless/native reopen before and after forced `.shm` rebuild; upstream blocked-function cases for MyLite-trimmed `GET_LOCK()`, `SLEEP()`, and `UUID_SHORT()` remain under the server-utility SQL function policy because they are rejected before generated-column validation; ownerless policy coverage verifies MariaDB errno 1903 and side-effect-free failure for create-time, replacement, and existing-column generated-column primary-key DDL, errno 1901 and side-effect-free failure for representative nondeterministic stored generated expressions, aggregate/subquery/time/session/nondeterministic plus retained crypto, statement-state, and user/version blocked-function classes, and index DDL over nondeterministic or session-dependent virtual generated expressions, and errno 1062 side-effect-free failure when replacing stored or virtual generated expressions would violate existing unique generated-column indexes; hook-build crash coverage kills representative failed generated-column `CREATE TABLE`, `ALTER TABLE`, and generated-column primary-key writers after MariaDB validation failure plus successful generated-column `CREATE TABLE`, generated-column `ALTER TABLE ... ADD COLUMN`, generated-column secondary-index, generated-column child/referenced-column `ALTER TABLE ... ADD CONSTRAINT` FK writers after native DDL completion, generated-column child/referenced-column `ALTER TABLE ... DROP FOREIGN KEY` FK writers after native metadata removal but before ownerless dictionary finish, and generated-column FK parent-delete writers immediately before InnoDB executes child-side cascade actions, after a successful child-side cascade returns before parent statement commit, inside `row_upd_step()` before the child-table update/delete is applied, or after the first, second, third, or sixth matching child-side `row_upd()` succeeds in a multi-child cascade, verifying no rejected native metadata leaks, stable retry errno 1901/1903, recovered generated-column metadata, generated values, forced generated-column index reads, generated-column FK enforcement or absence, retryable cascades, ownerless/native reopen, and forced `.shm` rebuild; exhaustive retained-function blocked-function matrices, exhaustive online-option matrices, exhaustive generated-column FK partial child-row crash matrices beyond the deterministic sixth-row boundary, and external oracle stress remain planned |
| Ownerless foreign-key action crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills ordinary and generated-column FK action writers before child-side cascade execution, after successful child-side action return before parent statement commit, before a child-side `row_upd()` call inside `row_upd_step()`, and after the first, second, third, or sixth matching child-side `row_upd()` succeeds in a multi-child cascade. Recovery verifies live-peer cleanup remains busy, no-live recovery rolls back parent/child changes, retries succeed, and final ownerless/native reopen before and after forced `.shm` rebuild observes the expected CASCADE and SET NULL state; exhaustive later-row fault selection beyond the deterministic sixth-row boundary, graph-wide randomized crash matrices, and long-running external MariaDB/RQG stress remain planned |
| `CREATE TABLE ... LIKE` | 🟡&nbsp;Partial | Representative MariaDB table-definition copy behavior is covered for default-engine tables, including ownerless peer visibility from an already-open handle, hook-build crash recovery after native destination table creation plus `CREATE OR REPLACE TABLE ... LIKE` replacement-copy completion but before ownerless dictionary finish, and no-live stale-reader replay for `CREATE OR REPLACE TABLE ... LIKE` preserving the copied-shape replacement tablespace final state with page-0 space-id identity checks |
| `CREATE TABLE ... SELECT` | 🟡&nbsp;Partial | Representative CTAS behavior is covered over MyLite tables, including ownerless peer visibility from an already-open handle, hook-build crash recovery after native destination table creation/population plus `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy completion but before ownerless dictionary finish, and no-live stale-reader replay for `CREATE OR REPLACE TABLE ... AS SELECT` preserving the populated replacement tablespace final state with page-0 space-id identity checks |
| Schemas/databases | 🟡&nbsp;Partial | Controlled `CREATE DATABASE`, qualified table access, and `DROP DATABASE` lifecycle are covered inside `datadir/`; ownerless coverage verifies peer refresh for a schema and InnoDB table created by another process, schema default charset/collation creation and `ALTER DATABASE` refresh through MariaDB's native `db.opt` file, idempotent `CREATE SCHEMA IF NOT EXISTS` / `CREATE DATABASE IF NOT EXISTS` and `DROP SCHEMA IF EXISTS` spellings including no-op duplicate-create/default-preservation and absent-drop behavior, peer-visible schema drop, final absent-schema state through ownerless/native reopen before and after forced `.shm` rebuild, hook-build crash recovery for killed `CREATE DATABASE` after native schema directory/`db.opt` creation, killed `ALTER DATABASE` after native `db.opt` rewrite, killed duplicate `CREATE DATABASE IF NOT EXISTS` preserving original defaults, killed missing `DROP SCHEMA IF EXISTS` preserving real schema/table state and missing-schema absence, and killed `DROP DATABASE` after native schema/table removal, each before ownerless dictionary finish, and retained-WAL stale-reader schema-drop replay preserving dropped schema and multi-table absence through ownerless/native reopen before and after forced `.shm` rebuild; broader schema behavior remains planned |
| Sequences | 🟡&nbsp;Partial | Simple MariaDB `CREATE SEQUENCE ... NOCACHE`, `NEXT VALUE FOR`, and `DEFAULT NEXTVAL()` behavior is covered across close and reopen in ordinary exclusive embedded mode; ownerless read/write mode rejects sequence DDL, direct and prepared top-level sequence value access before prepared-statement allocation, and hidden sequence expression execution from existing metadata before mutating sequence state; broader sequence DDL, ownerless sequence coordination, and edge cases remain planned |
| Representative application schemas | 🟡&nbsp;Partial | WordPress-shaped InnoDB `wp_options`, `wp_posts`, and `wp_postmeta` DDL and queries are covered as representative application-schema evidence |
| Views, triggers, and routines | 🟡&nbsp;Partial | Minimal `mysql.proc` / `mysql.procs_priv` metadata is initialized inside the MyLite directory, simple result-returning direct stored-procedure create, show, call, and drop behavior is covered, simple view create/query/drop behavior is covered including ownerless peer refresh and no-live ownerless/native reopen after forced `.shm` rebuild over an InnoDB base table, hook-build crash coverage kills simple `CREATE VIEW` and `DROP VIEW` writers after native view definition-file creation/removal plus `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view definition rewrite, explicit column-list create/replace/alter writers after native alias metadata storage/rewrite, check-option create/replacement/alter writers after native check-option metadata storage/rewrite, nested check-option outer-replacement and inner-alter writers after native nested view definition rewrite, explicit definer create writers after native security metadata storage, invoker replacement writers after native security metadata rewrite, and definer-security ALTER writers after native security metadata rewrite but before ownerless dictionary finish, then verifies recovered present/absent, rewritten, column-list, check-option, nested check-option, or security view metadata, `.frm` file state, view query behavior, old exposed-column rejection, base-table writes, and ownerless/native reopen before and after forced `.shm` rebuild, ownerless `CREATE OR REPLACE VIEW` and `ALTER VIEW` definition and explicit column-list refresh are covered for already-open peers with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless view idempotent DDL covers `CREATE VIEW IF NOT EXISTS`, duplicate-create errno 1050, no-op duplicate definition preservation, repeated `DROP VIEW IF EXISTS`, and hook-build crash recovery for duplicate `CREATE VIEW IF NOT EXISTS` plus missing `DROP VIEW IF EXISTS` no-op writers preserving the original view definition and missing-view absence, with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless updatable view `WITH LOCAL/CASCADED CHECK OPTION` metadata refresh and valid/invalid DML through the view are covered for already-open peers with MariaDB errno 1369 on check-option failures and final ownerless/native reopen before and after forced `.shm` rebuild, prepared `SELECT`, `INSERT`, and `UPDATE` through an ownerless check-option view are covered for already-open peers, including MariaDB errno 1369 on invalid prepared insert/update attempts, prepared DML reuse after peer view replacement, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless non-updatable aggregate-view diagnostics cover `IS_UPDATABLE = 'NO'`, errno 1471 for `INSERT`, errno 1288 for `UPDATE`/`DELETE`, errno 1368 for rejected `WITH CHECK OPTION`, direct failed-write immutability, ownerless prepared-DML step-time errno 1471 for prepared `INSERT`, ownerless prepared-DML step-time errno 1288 for prepared `UPDATE`/`DELETE`, peer replacement refresh, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless invalid view dependency diagnostics cover MariaDB errno 1356 after a peer drops the base table, recovery after the peer recreates that base table, and final ownerless/native reopen before and after forced `.shm` rebuild, ownerless nested updatable view coverage verifies outer `LOCAL` versus `CASCADED` propagation over an inner check-option view plus inner-view predicate refresh with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless view security/definer coverage verifies `DEFINER=CURRENT_USER`, `SQL SECURITY DEFINER`, and `SQL SECURITY INVOKER` metadata refresh for already-open peers with final ownerless/native reopen before and after forced `.shm` rebuild, simple trigger create/fire/drop behavior is covered with ownerless peer refresh and no-live ownerless/native reopen after forced `.shm` rebuild over InnoDB base/audit tables, ownerless trigger variants cover `BEFORE UPDATE` `NEW` mutation, `CREATE OR REPLACE TRIGGER` replacement, and `AFTER DELETE` `OLD` audit effects through an already-open peer with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless trigger ordering covers `FOLLOWS`/`PRECEDES` `ACTION_ORDER`, firing order, and `SHOW CREATE TRIGGER` lookup by trigger name through an already-open peer with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless trigger idempotent DDL covers `CREATE TRIGGER IF NOT EXISTS`, duplicate-create errno 1359, no-op duplicate preservation, and repeated `DROP TRIGGER IF EXISTS` with final ownerless/native reopen before and after forced `.shm` rebuild, ownerless stored routine DDL (`CREATE`/`ALTER`/`DROP FUNCTION`, `PROCEDURE`, `PACKAGE`, or `PACKAGE BODY`) is rejected before mutating `mysql.proc`, and ownerless direct/prepared `CALL`, direct/prepared stored-function expression execution, and trigger-body stored procedure/function execution are rejected before routine-body effects mutate InnoDB rows; ownerless stored routine DDL support, ownerless routine execution support, view privilege/security semantics, invalid view definers, broader view semantics, broader trigger edge cases, packages, routine edge cases, empty-result metadata, and metadata compatibility remain planned |
| Trigger DDL crash recovery | 🟡&nbsp;Partial | Hook-build crash coverage kills simple `CREATE TRIGGER` and `DROP TRIGGER`, `CREATE OR REPLACE TRIGGER`, ordered `CREATE TRIGGER ... PRECEDES ...`, duplicate `CREATE TRIGGER IF NOT EXISTS`, missing `DROP TRIGGER IF EXISTS`, delayed missing-dependency `CREATE TRIGGER`, explicit `CREATE DEFINER=CURRENT_USER TRIGGER`, and stored-function-body `CREATE TRIGGER` writers after native `.TRG`/`.TRN` metadata creation/removal, rewrite, no-op preservation, delayed dependency acceptance, or definer metadata storage but before ownerless dictionary finish, then verifies live-peer cleanup remains busy until no-live recovery, recovered present/absent `INFORMATION_SCHEMA.TRIGGERS` metadata, native trigger-file state, trigger firing after recovered create, replacement, idempotent no-op preservation, delayed dependency creation, definer recovery, stored-function trigger metadata recovery, ownerless stored-routine execution rejection before base-row mutation, and ordinary native stored-function trigger firing, recovered MariaDB 1146 failure when the missing dependency is absent, recovered `ACTION_ORDER`/firing order after ordered create, non-firing base-table inserts after recovered drop, `SHOW CREATE TRIGGER` for recovered present triggers including explicit definer metadata and rejection for the dropped trigger, ownerless/native reopen before and after forced `.shm` rebuild, and absent missing-trigger `.TRN` state after recovered missing drop; broader privilege/security and randomized trigger crash variants remain planned |
| Events and scheduler | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server scheduler is not part of the core embedded profile; event DDL, event metadata commands, and scheduler variables are rejected by direct and prepared policy coverage, including ownerless read/write coverage that verifies rejected event metadata stays absent across ownerless/native reopen before and after forced `.shm` rebuild, and the default embedded archive uses only a parser-link event parse-data stub |
| Users, grants, and password auth | ➖&nbsp;Out&nbsp;of&nbsp;scope | Local embedded directory ownership replaces server account management; account, role, grant, revoke, and password statements are rejected by policy coverage, and the default embedded archive omits the `unix_socket` server auth plugin |
| Foreign-server metadata | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server-global remote connection metadata; `CREATE SERVER`, `CREATE OR REPLACE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` are rejected by policy coverage, and the default embedded archive omits the `mysql.servers` metadata cache |
| Replication and binlog | ➖&nbsp;Out&nbsp;of&nbsp;scope | Server topology feature, not core library behavior; replication and binlog command families, SQL `BINLOG` replay, GTID helper functions, GTID state variable assignments, and binary-log GTID-index tuning variables are rejected or omitted, `@@log_bin=0` is covered, and the default embedded archive omits the active binlog transaction/event core, SQL `BINLOG` replay source, server event writers, binary-log event parser/reader runtime, replication GTID-state runtime, binary-log GTID-index runtime, residual replication helper objects, unsupported injector root, guarded replication execution system variables, and replication/binlog filter runtime |
| External XA transactions | ➖&nbsp;Out&nbsp;of&nbsp;scope | Distributed transaction-manager surface; direct and prepared `XA` statements are rejected by policy coverage, and the default embedded archive omits the external-XA runtime plus the mmap-backed `tc.log` transaction coordinator while ordinary native-engine transactions remain covered |
| SQL `HANDLER` commands | ➖&nbsp;Out&nbsp;of&nbsp;scope | Low-level server table-cursor surface; direct and prepared top-level `HANDLER ...` statements are rejected by policy coverage, and the default embedded archive omits SQL `HANDLER` command runtime while retaining MariaDB's storage-engine `handler` abstraction |
| Host-file SQL exports | ➖&nbsp;Out&nbsp;of&nbsp;scope | `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` write arbitrary host files outside result delivery; direct and prepared forms are rejected by policy coverage, ownerless retained-WAL pressure coverage verifies representative direct, CTE, and prepared export diagnostics are not masked by `MYLITE_BUSY`, and the default embedded archive omits the host-file writer bodies while retaining `SELECT ... INTO` variables |
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
| Foreign keys | 🟡&nbsp;Partial | Representative InnoDB foreign-key enforcement and cascade delete behavior are covered; ownerless coverage verifies peer refresh for create-time foreign keys plus `ALTER TABLE` foreign-key add/drop with missing-parent enforcement before drop and orphan insertion after drop, cross-process referential actions for `ON UPDATE CASCADE`, `ON DELETE CASCADE`, `ON DELETE SET NULL`, and `ON DELETE RESTRICT`, native FK checks now prepare ownerless current-read visibility and reopen parent/child FK cursors so peer-created child rows and clustered child records are resolved without SQL-level metadata probes, hook-build crash recovery for ordinary parent update/delete writers killed immediately before InnoDB executes child referential actions, after one successful ordinary child action returns before parent statement commit, inside `row_upd_step()` before ordinary child update/delete application, and after the first, second, third, or sixth matching ordinary child-side `row_upd()` succeeds in a multi-child cascade, plus generated-column FK parent-delete writers killed immediately before InnoDB executes child referential actions, after a successful generated-column child cascade returns before parent statement commit, inside `row_upd_step()` before child update/delete application, and after the first, second, third, or sixth matching generated-column child-side `row_upd()` succeeds in a multi-child cascade, tenant-scoped composite foreign-key enforcement/cascade/restrict behavior, multi-hop `ON UPDATE`/`ON DELETE CASCADE` chains through four InnoDB tables, supported stored generated-column child and referenced-column foreign-key shapes plus indexed virtual generated child create/alter shapes with `ON UPDATE RESTRICT`/`ON DELETE CASCADE`, MariaDB-rejected generated-column action clauses with errno 1905, hook-build crash recovery for generated-column child and referenced-column `ALTER TABLE ... ADD CONSTRAINT` and `ALTER TABLE ... DROP FOREIGN KEY` FK writers killed after native metadata creation/removal but before ownerless dictionary finish, two-table and three-table cyclic `ON DELETE CASCADE`, cyclic `ON DELETE SET NULL`, and MariaDB-native cyclic update rejection, same-schema parent-table `RENAME TABLE` metadata refresh, child-table `RENAME TABLE` refresh for generated `<child>_ibfk_1` constraint names, cross-schema parent-table `RENAME TABLE` refresh with `UNIQUE_CONSTRAINT_SCHEMA` movement, cross-schema child-table `RENAME TABLE` refresh with `CONSTRAINT_SCHEMA` and generated `<child>_ibfk_1` movement, same-schema plus cross-schema multi-pair parent/child `RENAME TABLE` refresh with valid-child insertion, missing-parent rejection, restricted-delete rejection, and ownerless/native reopen before and after forced `.shm` rebuild, and opt-in deterministic ownerless foreign-key graph stress over concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers with bounded retry for MariaDB 1205/1213, transient page-write commit-boundary coverage plus transaction page LSN coverage and DML current-read refresh for parent clustered/secondary index pages, errno 1451/1452 checks, ownerless/native reopen before and after forced `.shm` rebuild, deterministic SQL trace export for external harness input, and seeded FK graph trace-suite validation for deterministic external-oracle variants; full external MariaDB/RQG FK graph execution and randomized later-child-row intra-action FK graph crash edge cases remain planned |
| Ownerless FK multi-rename crash coverage | 🟡&nbsp;Partial | Hook-build coverage kills same-schema parent-through-temporary plus cross-schema parent/child foreign-key multi-pair `RENAME TABLE` writers after native InnoDB FK metadata rewrite and `.frm`/`.ibd` movement but before ownerless dictionary finish, proves live-peer cleanup remains busy, verifies no-live recovery of moved parent/child metadata, moved generated `<child>_ibfk_1` identity, same-schema temporary-parent absence, cross-schema target-schema files, valid child inserts, missing-parent errno 1452, restricted-delete errno 1451, and ownerless/native reopen before and after forced `.shm` rebuild; native-loop crash injection and broader DDL/file-lifecycle recovery remain planned |
| Ownerless FK graph external replay | 🟡&nbsp;Partial | `tools/ownerless-fk-graph-trace` emits bounded `1205`/`1213` plus SQLSTATE `40001` retry procedures around each deterministic FK graph worker transaction so a raw external `mariadb` client can replay concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers through the Docker-backed trace smoke; seed `0` preserves the default deterministic graph schedule, nonzero seeds generate deterministic external-oracle delta variants, `tools/ownerless-fk-graph-seed-suite` validates or replays multiple seeded traces through the common trace runner with bounded whole-seed replay attempts for transient external `1205`/`1213` escapes, and `tools/ownerless-external-mariadb-seed-sweep` now includes FK graph in its combined check/replay plan with a manifest-recorded FK replay-attempt budget; Docker-backed MariaDB 11.8 replay passed seeds `0`, `17`, `83`, and `211` plus contiguous seed windows `16` through `31`, `32` through `63`, `64` through `95`, `96` through `127`, `128` through `159`, `160` through `191`, `192` through `223`, `224` through `255`, `256` through `287`, `288` through `319`, `320` through `351`, `352` through `383`, `384` through `415`, `416` through `447`, `448` through `479`, `480` through `511`, `512` through `543`, `544` through `575`, `576` through `607`, `608` through `639`, `640` through `671`, `672` through `703`, `704` through `735`, `736` through `767`, `768` through `799`, `800` through `831`, `832` through `863`, `864` through `895`, `896` through `927`, `928` through `959`, `960` through `991`, `992` through `1023`, `1024` through `1055`, `1056` through `1087`, `1088` through `1119`, and `1120` through `1151` at rounds `2`, with whole-seed retries recovering transient raw MariaDB `1213` exits; the `224` through `255` window used an explicit FK retry budget of `10` and recovered seeds `229`, `232`, `233`, `234`, `239`, `242`, and `252`, the `256` through `287` window used the same budget and recovered seeds `260`, `267`, `277`, and `280`, the `288` through `319` window used the same budget and recovered seeds `288`, `294`, `298`, `304`, `308`, and `310`, the `320` through `351` window used the same budget and recovered seed `344` on attempt `3` plus seed `348` on attempt `2`, the `352` through `383` window used the same budget and recovered seed `354` on attempt `4`, seeds `357` and `365` on attempt `3`, plus seeds `358`, `362`, `363`, `368`, `369`, `371`, `375`, and `379` on attempt `2`, the `384` through `415` window used the same budget and recovered seed `403` on attempt `3`, seed `405` on attempt `2`, seed `406` on attempt `5`, seed `408` on attempt `3`, and seed `413` on attempt `2`, the `416` through `447` window used the same budget and recovered seed `417` on attempt `3`, seeds `418` and `419` on attempt `2`, seed `420` on attempt `5`, seed `423` on attempt `3`, and seeds `425`, `434`, and `436` on attempt `2`, the `448` through `479` window used the same budget and recovered seed `449` on attempt `3`, seed `456` on attempt `2`, seeds `463`, `466`, `467`, `471`, and `475` on attempt `3`, and seed `477` on attempt `2`, and the `480` through `511` window used the same budget and recovered seed `481` on attempt `2`, seed `483` on attempt `3`, seed `486` on attempt `2`, seed `490` on attempt `5`, seed `492` on attempt `2`, seed `493` on attempt `4`, seed `500` on attempt `4`, seed `503` on attempt `2`, seed `505` on attempt `3`, seeds `507`, `509`, and `510` on attempt `2`, seed `511` on attempt `3`, the `512` through `543` window used the same budget and recovered seed `519` on attempt `3`, plus seeds `522`, `523`, `525`, `527`, `528`, `536`, and `542` on attempt `2`, the `544` through `575` window used the same budget and recovered seed `545` on attempt `5`, seed `548` on attempt `4`, seeds `549` and `557` on attempt `3`, plus seeds `546`, `560`, `562`, `564`, and `575` on attempt `2`, the `576` through `607` window used the same budget and recovered seeds `581`, `586`, and `605` on attempt `4`, seeds `576`, `584`, `595`, `599`, and `604` on attempt `3`, plus seeds `578`, `579`, `582`, `587`, `589`, `590`, and `606` on attempt `2`, the `608` through `639` window used the same budget and recovered seeds `609`, `613`, and `621` on attempt `3`, plus seeds `608`, `616`, `618`, `626`, `633`, and `634` on attempt `2`, the `640` through `671` window used the same budget and recovered seed `657` on attempt `4`, seed `654` on attempt `3`, plus seeds `641`, `644`, `647`, `649`, `652`, `655`, `667`, `669`, and `670` on attempt `2`, the `672` through `703` window used the same budget and recovered seed `701` on attempt `6`, seed `689` on attempt `5`, seed `678` on attempt `4`, seeds `693`, `698`, `699`, `700`, and `702` on attempt `3`, plus seeds `672`, `692`, `694`, and `703` on attempt `2`, and the `704` through `735` window used the same budget and recovered seed `722` on attempt `5`, seeds `708`, `725`, and `726` on attempt `3`, plus seeds `704`, `706`, `713`, `715`, `718`, `724`, `728`, and `732` on attempt `2`, the `736` through `767` window used the same budget and recovered seeds `737`, `738`, `755`, and `764` on attempt `4`, seeds `742`, `745`, and `747` on attempt `3`, plus seeds `736`, `744`, `746`, `750`, `757`, and `765` on attempt `2`, the `768` through `799` window used the same budget and recovered seed `778` on attempt `4`, seeds `774`, `780`, and `788` on attempt `3`, plus seeds `769`, `777`, `785`, `787`, and `794` on attempt `2`, the `800` through `831` window used the same budget and recovered seeds `801` and `803` on attempt `4`, seeds `811` and `829` on attempt `3`, plus seeds `802`, `806`, `823`, `826`, and `830` on attempt `2`, and the `832` through `863` window used the same budget and recovered seed `849` on attempt `4`, seeds `834` and `852` on attempt `3`, plus seeds `833`, `837`, `845`, `846`, and `851` on attempt `2`, the `864` through `895` window used the same budget and recovered seed `887` on attempt `5`, seed `872` on attempt `3`, plus seeds `865`, `870`, `878`, `879`, `880`, `886`, and `891` on attempt `2`, and the `896` through `927` window used the same budget and recovered seed `905` on attempt `5`, seeds `904`, `910`, and `918` on attempt `3`, plus seeds `899` and `908` on attempt `2`, and the `928` through `959` window used the same budget and recovered seed `957` on attempt `4`, seed `939` on attempt `3`, plus seeds `932`, `934`, `946`, and `956` on attempt `2`, the `960` through `991` window used the same budget and recovered seed `968` on attempt `5`, seeds `963`, `969`, and `985` on attempt `4`, seeds `965`, `972`, `976`, `979`, and `984` on attempt `3`, plus seeds `975`, `978`, `981`, and `982` on attempt `2`, the `992` through `1023` window used the same budget and recovered seeds `992`, `993`, `994`, `997`, `999`, `1004`, `1011`, `1015`, `1020`, and `1022` on attempt `2`, the `1024` through `1055` window used the same budget and recovered seed `1052` on attempt `7`, seeds `1026` and `1054` on attempt `3`, plus seeds `1029`, `1031`, `1042`, `1043`, `1053`, and `1055` on attempt `2`, and the `1056` through `1087` window used the same budget and recovered seeds `1067` and `1074` on attempt `3`, plus seeds `1060`, `1063`, `1068`, `1070`, `1078`, and `1080` on attempt `2`, and the `1088` through `1119` window used the same budget and recovered seed `1104` on attempt `6`, seeds `1096` and `1097` on attempt `5`, seed `1114` on attempt `4`, seeds `1095` and `1103` on attempt `3`, plus seeds `1088`, `1094`, `1105`, `1108`, `1111`, and `1112` on attempt `2`, and the `1120` through `1151` window used the same budget and recovered seeds `1120`, `1121`, and `1130` on attempt `3`, plus seeds `1123`, `1128`, `1133`, `1136`, `1150`, and `1151` on attempt `2`; long-running randomized MariaDB/RQG FK graph execution and deeper intra-action FK graph crash coverage remain planned |
| Ownerless random transaction trace export | 🟡&nbsp;Partial | `tools/ownerless-random-tx-trace` emits the deterministic ownerless random transaction schedule as schema, per-worker SQL, manifest metadata, and final aggregate oracles; seed `0` preserves the product C stress formulas, while nonzero seeds generate deterministic external-oracle variants for row choice, rollback points, and update deltas. `tools/ownerless-random-tx-seed-suite` validates or replays multiple seeded traces through the common trace runner, with dependency-free CTest coverage and focused Docker-backed MariaDB 11.8 replay evidence for seeds `0`, `17`, `83`, and `211`; long-running randomized MariaDB/RQG transaction generation remains planned |
| Generated columns | 🟡&nbsp;Partial | Representative stored and virtual generated-column behavior is covered through native storage support; ownerless coverage verifies peer refresh for create-time generated columns plus `ALTER TABLE` generated-column add/drop, same-kind stored/virtual expression replacement, and indexed same-kind expression replacement with generated expression reads, forced-index reads, base-column writes, and reopen checks; ownerless policy coverage preserves MariaDB's errno 1901 rejection for representative nondeterministic stored generated expressions plus aggregate, subquery, time-dependent, session-dependent, nondeterministic, retained crypto, statement-state, and user/version blocked-function classes, and hook-build crash coverage proves representative failed generated-column DDL leaves clean metadata after recovery |
| Generated-column indexes | 🟡&nbsp;Partial | Ownerless coverage verifies stored and virtual generated-column secondary-index create/use/drop refresh, unique generated-column indexes, prefix generated-column indexes, mixed-direction composite generated-column indexes, accepted explicit `NOCOPY`/`INPLACE` generated-column index add/drop options, forced-index reads, generated-value recalculation after base-column DML, and indexed generated-expression replacement for ordinary stored and virtual generated-column secondary indexes; ownerless policy coverage preserves MariaDB's generated-column primary-key rejection with errno 1903, generated-column-function rejection with errno 1901 for indexes over nondeterministic and session-dependent virtual generated expressions, and duplicate-key rejection with errno 1062 for generated-expression replacement that would violate existing unique generated-column indexes; representative failed and successful generated-column DDL crash recovery plus pre-child-action, post-child-action, row-step-before-update, and first-, second-, third-, plus sixth-row after-update generated-column FK action crash recovery are covered, while exhaustive retained-function blocked-function, exhaustive online-option, broader generated-column FK partial child-row crash matrices, and external-oracle matrices remain planned |
| FULLTEXT, SPATIAL, and vector indexes | ⚪&nbsp;Planned | Support only where the selected native engine and embedded profile support them; ownerless read/write mode rejects top-level, ALTER, inline, and idempotent `FULLTEXT` and `SPATIAL` index DDL until full-text auxiliary state, spatial R-tree pages, and spatial predicate-lock coordination are designed, and vector index DDL is rejected by the default no-vector profile |

## Transactions, Recovery, And Concurrency

| Capability | MyLite status | Compatibility target |
| --- | --- | --- |
| Atomic commit | 🟡&nbsp;Partial | Explicit InnoDB transactions commit through native MariaDB/InnoDB hooks inside the MyLite database directory; ownerless coverage pauses multiple independent-table transactions before commit, releases the commits together, and verifies every delta is durable |
| Rollback | 🟡&nbsp;Partial | Explicit InnoDB transaction rollback is covered; MyISAM remains non-transactional |
| Savepoints | 🟡&nbsp;Partial | Explicit InnoDB savepoint rollback and release savepoint are covered through SQL transaction statements |
| Crash recovery | 🟡&nbsp;Partial | Parent-process reopen after child-process exit covers committed InnoDB rows surviving and uncommitted rows rolling back |
| Same-process multi-handle concurrency | 🟡&nbsp;Partial | Multiple `mylite_db` handles over one embedded runtime are covered for committed visibility, simultaneous active InnoDB transactions on different rows, row-lock timeout behavior, shared wait-edge publication for local InnoDB row waits, metadata-lock timeout behavior, savepoints, and foreign-key enforcement |
| Multiple readers | 🟡&nbsp;Partial | Ownerless read/write opens and `MYLITE_OPEN_READONLY \| MYLITE_OPEN_SHARED_READONLY` handles can read committed InnoDB updates from peer processes through native-file refresh and safe page-version reads, including prepared `SELECT` execution, shared read-only repeatable-read snapshots while a peer ownerless writer commits, tested read-only transaction first-read/repeatable-snapshot behavior, `READ COMMITTED` transaction reads that observe a later peer commit, and reads inside transactions after local writes, including local-write plain reads before a local-native read watermark exists; ownerless `READ UNCOMMITTED` isolation requests are rejected before unproven cross-process dirty reads; stale ownerless coordination that needs recovery must be reopened read/write before shared read-only handles can attach |
| Concurrent writers | 🟡&nbsp;Partial | Ownerless cross-process read/write opens coordinate InnoDB row/table locks, gap/next-key locks that block peer inserts and allow post-release retry, serializable read locks that block peer writers, a serializable write-skew candidate where two predicate readers cannot both commit disjoint updates, transaction and savepoint-rollback visibility, concurrent explicit commit visibility through live ownerless opens, forced shared-memory rebuild, and native exclusive reopen for the covered explicit-commit race, DDL/DML stress, temporary-table stress, checksum-oracle stress, explicit transaction/savepoint stress, pseudo-random transaction stress, and foreign-key graph stress shapes, redo visibility, targeted post-wait page refresh, representative metadata-lock blocking, concurrent DDL table/space/index metadata allocation including online index replacement, peer-refresh visibility for foreign-key, generated-column, idempotent table create/drop, `CREATE TABLE ... LIKE`, CTAS, online/in-place index DDL, table charset conversion, row-format rebuild, table comment metadata, `ALTER TABLE ... FORCE` rebuild, column default set/drop, column idempotent add/drop, standalone `CREATE INDEX`/`DROP INDEX` including idempotent secondary-index create/drop and ordinary inline `CREATE TABLE ... INDEX` create/duplicate-failure semantics, multi-column, unique descending, unique prefix, unique prefix-plus-direction, utf8mb4 prefix, unique TEXT/BLOB prefix, and unique TEXT/BLOB prefix-plus-direction secondary-index enforcement before drop, and descending, mixed-direction, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix, TEXT/BLOB prefix-plus-direction, and prefix-plus-direction key-part metadata, secondary-index rename and ignored/not-ignored metadata, primary-key idempotent ADD no-op preservation plus replacement with duplicate enforcement on the new key, descending primary-key replacement direction metadata and duplicate enforcement, composite direction primary-key replacement metadata and duplicate enforcement, AUTO_INCREMENT descending primary-key replacement metadata and duplicate allocation gap enforcement, foreign-key ALTER add/drop enforcement, cross-process foreign-key referential actions, composite foreign-key enforcement/cascade/restrict behavior, deep foreign-key cascade-chain update/delete behavior, stored/virtual generated-column foreign-key enforcement/restrict/cascade behavior plus MariaDB-rejected generated-column action policy, cyclic foreign-key cascade/set-null/rejected-update behavior, same-schema foreign-key parent-table/child-table rename metadata/enforcement, cross-schema foreign-key parent-table/child-table rename metadata/enforcement, same-schema and cross-schema foreign-key multi-pair parent/child rename metadata/enforcement, CHECK constraint ALTER add/drop enforcement, generated-column ALTER add/drop and same-kind expression-replacement refresh, column add/modify/rename/drop ALTERs, explicit InnoDB instant ADD/DROP/reorder column metadata plus instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants refresh, schema create/drop lifecycle refresh, schema default charset/collation DDL refresh plus schema-default rewrite crash recovery, cross-schema InnoDB table rename and multi-pair rename-cycle refresh with ownerless/native reopen before and after forced `.shm` rebuild, view create/query/drop, replacement/alter, idempotent create/drop, column-list, check-option, prepared check-option DML, direct/prepared non-updatable view diagnostics, invalid view dependency diagnostics, nested check-option, and security/definer metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, trigger create/fire/drop, replacement/update/delete, ordering/show-create, and idempotent create/drop metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, ownerless stored routine DDL rejection before uncoordinated `mysql.proc` writes, including MariaDB package specification/body routine rows, and ownerless top-level `CALL`, stored-function expression, and trigger-body procedure/function execution rejection before routine-body effects mutate InnoDB rows, ownerless top-level sequence SQL plus hidden `DEFAULT NEXTVAL()` execution rejection before uncoordinated sequence-table writes, ownerless table-admin SQL rejection before uncoordinated checksum reads, statistics, upgrade-check, repair, or admin-rebuild paths, ownerless `LOCK TABLES` rejection before unproven SQL locked-table mode, ownerless `FLUSH TABLES ... WITH READ LOCK`/`FOR EXPORT` rejection before unproven global read-lock, locked-table, and export/quiesce paths, ownerless `READ UNCOMMITTED` isolation rejection before unproven cross-process dirty reads, ownerless table `DATA DIRECTORY`/`INDEX DIRECTORY` option rejection before external file lifecycle paths, ownerless top-level, ALTER, inline, and idempotent `FULLTEXT`/`SPATIAL` index DDL rejection before unproven special-index storage writes, ownerless partitioned table DDL rejection before unproven partition file-lifecycle writes, no-live ownerless/native exclusive reopen of the final broader DDL state before and after forced `.shm` rebuild, no-live stale-reader rebuild discard for retained reader-boundary WAL covering single-table dropped, same-schema and cross-schema same-statement multi-dropped, ordinary-created, LIKE-copy, CTAS-created, recreated, renamed, truncated, force-rebuilt, and multi-rename-swap file-per-table final states plus multi-table schema-drop absence, dirty no-live recovery skip for retained WAL records whose dropped file-per-table tablespace no longer exists, and large-table reuse after peer truncate, local DDL readability after dictionary flush, concurrent same-named InnoDB temporary tables, killed temporary-table peer cleanup while another temp-table peer remains live, shared AUTO_INCREMENT reservation across concurrent ownerless insert workers, ownerless AUTO_INCREMENT DDL high-watermark refresh for already-open peers, ownerless AUTO_INCREMENT column-add rebuild refresh for already-open peers, multi-object reader/writer stress, opt-in high-pressure ownerless independent-table stress plus deterministic independent-table stress SQL trace export for external harness input, opt-in DDL/DML stress plus deterministic DDL stress and DDL lifecycle SQL trace export for external harness input, opt-in temporary-table stress plus deterministic temporary-table stress SQL trace export for external harness input, opt-in explicit transaction/savepoint stress plus deterministic transaction stress SQL trace export for external harness input, opt-in checksum-oracle stress over one shared table plus deterministic checksum stress SQL trace export for external harness input, opt-in pseudo-random shared-table transaction stress with bounded retry after MariaDB lock-wait/deadlock errors plus a deterministic SQL trace exporter for external oracle runners, opt-in foreign-key graph stress with bounded retry after MariaDB lock-wait/deadlock errors while concurrent `CASCADE`, `SET NULL`, and `RESTRICT` workers mutate one graph plus transient page-write commit-boundary, transaction page LSN coverage, and DML current-read refresh plus a deterministic FK graph SQL trace exporter for external harness input, opt-in active-reader pressure stress under a repeatable-read snapshot pin plus deterministic active-reader pressure SQL trace export for external harness input, opt-in ownerless page-version WAL pressure limits for active-reader pins across direct/prepared writes, representative DML/DDL write classes, AUTO_INCREMENT DDL high-watermark ALTER, variant DML/index/rename/truncate spellings, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants, safe page-version reads inside mutating transactions, no-live-process page-version replay to existing tablespaces, retained page-version WAL for native exclusive reopen, no-peer and zero-active-pin live-peer native checkpoint reclamation including dead snapshot-pin cleanup and partial compaction while newer records remain, native boundary synthesis for active readers with product WAL retention until pins release, plus primitive active-pin page-version compaction evidence, native boundary synthesis when an older on-disk page image is still readable at page-version publish time, active snapshot-pin blocking of product live-peer prefix compaction until release when synthesis cannot prove a boundary, thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling when no peer process is live plus live-peer gating coverage before close-time reclaim, timer-driven checkpoint scheduling after shared read-only snapshot release without another writer SQL statement, bounded repeated same-row writer pressure while a live reader pins its snapshot, and conservative same-page write serialization through directory-owned files; external SQL trace replay harnessing is covered for generated deterministic traces plus full-suite trace-runner validation, optional client replay, opt-in disposable MariaDB Docker smoke-safe subset tooling, and full scale-2 deterministic Docker-backed MariaDB replay, while full external MariaDB/RQG long-running stress remains planned |
| Cross-process unsafe writers | 🟡&nbsp;Partial | A second ordinary read/write process open is rejected with `MYLITE_BUSY` while another process owns the MyLite directory lock; ordinary exclusive opens still create fixed ownerless coordination headers but fresh ordinary opens stay on the native MariaDB SQL hot path without installing ownerless runtime lifecycle, MDL, transaction, read-view, or InnoDB hooks, and without appending ownerless page-version WAL payloads unless the handle is opened through `MYLITE_OPEN_OWNERLESS_RW`, `MYLITE_OPEN_SHARED_READONLY`, or retained ownerless WAL payload requires native exclusive replay |
| Ownerless lock fault coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills a blocked writer after it enters the external ownerless record wait but before MariaDB grants the local waiting record lock, and kills a blocked writer after MariaDB grants a waited record lock and MyLite publishes that granted lock to shared state, proving live-peer cleanup remains busy and no-live reopen preserves only committed data. Primitive coverage kills a process while a table-lock wait entry is live, verifies the wait entry remains observable after process death, and proves owner cleanup removes it; embedded hook coverage directly asserts retained external table-wait snapshots dispatch through `wait_until_table` with stable transaction ID, table ID, mode, timeout, and result propagation; hook-build SQL coverage now proves a `foreign_key_checks=0` and `unique_checks=0` empty-table bulk insert waiting behind a peer ownerless `LOCK IN SHARE MODE` reader publishes a shared external native table-wait registry entry, clears it after release, supports retry insert, and remains visible through ownerless/native reopen and forced `.shm` rebuild. Hook-build SQL crash coverage kills that same native table-wait SQL waiter after the shared table-wait entry is published, verifies the dead wait remains observable, verifies live-peer cleanup remains busy while the blocking reader is still alive, verifies no-live recovery removes the dead wait after the blocker dies, confirms the interrupted insert is absent, and retries the insert through ownerless/native reopen and forced `.shm` rebuild. Hook-build SQL negative proof still arms the local ownerless table-wait callback while representative blocked `ALTER TABLE`, instant add/rename column, column modify/default, table comment, CHECK/FK add, `CREATE INDEX`, unique and online index add, existing-index drop/rename/ignored, copy-force and primary-key replacement `ALTER TABLE`, charset conversion, row-format ALTER, `TRUNCATE TABLE`, `RENAME TABLE`, `DROP TABLE`, `CREATE OR REPLACE TABLE ... LIKE`, and `CREATE OR REPLACE TABLE ... AS SELECT` variants time out, verifies blocked metadata remains unchanged, and fails if any tested SQL shape reaches the local callback. Positive SQL-level local table-wait fault injection remains unclaimed beyond the covered external native table-wait registry path, while ownerless `LOCK TABLES`/`UNLOCK TABLES` is explicitly rejected until SQL locked-table mode is designed |
| Ownerless writer tests | 🟡&nbsp;Partial | Normal embedded builds run `MYLITE_OPEN_OWNERLESS_RW` cross-process SQL writer tests that bypass the process-wide directory lock and route through the ownerless process, transaction, read-view, InnoDB lock, redo-visibility, page-version WAL, and checkpoint hooks; tests cover non-conflicting writers, same-page writer serialization, concurrent explicit commits on independent tables, native exclusive reopen and recovery-anchor checks after the concurrent explicit-commit race before and after forced `.shm` rebuild, row waits, gap-lock insert blocking plus post-release retry, serializable reader/writer blocking, serializable write-skew prevention for disjoint predicate-dependent updates, savepoint rollback before peer-visible commit, deadlocks across separate tables, mixed readers/writers, shared AUTO_INCREMENT assignment across concurrently opened insert workers, ownerless AUTO_INCREMENT DDL high-watermark refresh, ownerless AUTO_INCREMENT column-add rebuild refresh, ownerless AUTO_INCREMENT primary-key replacement refresh, ownerless AUTO_INCREMENT descending primary-key replacement refresh, a bounded independent-table writer/multi-object reader stress loop, committed external visibility through native refresh, direct and prepared SELECT visibility, shared read-only prepared reads and read-only rejection for direct/prepared writes, transaction first-read visibility after peer commits, transaction reads after local writes, repeatable-read and `WITH CONSISTENT SNAPSHOT` retention across peer churn, session-scoped and transaction-scoped read-committed visibility of later peer commits, shared-memory rebuild, no-live-process page-version replay with retained WAL, no-live stale-reader rebuild over retained reader-boundary WAL for single-table dropped, same-schema and cross-schema same-statement multi-dropped, ordinary-created, LIKE-copy, CTAS-created, recreated, renamed, truncated, force-rebuilt, same-schema and cross-schema multi-rename-swap file-per-table tablespaces plus multi-table schema-drop absence, live idle-peer page-log reclamation, active snapshot-pin blocking of live-peer reclamation when boundary proof is missing, native boundary synthesis for a live snapshot pin, active-reader pressure limit throttling for direct/prepared writes including prepared `INSERT ... SELECT`, representative DML/DDL write classes, AUTO_INCREMENT DDL high-watermark ALTER, variant DML/index/rename/truncate spellings, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants, active-reader pressure diagnostics, active-pin boundary retention until release, killed snapshot-pin cleanup allowing live-peer reclamation, cross-process `ALTER TABLE` waiting, concurrent DDL table/space/index metadata allocation with online index replacement, peer-visible ownerless DDL for create, rename, truncate, post-truncate DML, large-table truncate reuse, drop, same-name recreate, idempotent table create/drop, `CREATE TABLE ... LIKE`, CTAS, standalone `CREATE INDEX`/`DROP INDEX` plus idempotent standalone, `ALTER TABLE`, and inline `CREATE TABLE` index create/duplicate-failure semantics, descending, mixed-direction, `VARCHAR`, utf8mb4, and TEXT/BLOB prefix, TEXT/BLOB prefix-plus-direction, and prefix-plus-direction key-part metadata refresh, secondary-index rename, secondary-index ignored/not-ignored metadata refresh, multi-column unique replacement/enforcement plus idempotent top-level and `ALTER TABLE` unique-index create/no-op/drop preservation, unique descending, unique prefix, unique prefix-plus-direction, utf8mb4 prefix, unique TEXT/BLOB prefix, and unique TEXT/BLOB prefix-plus-direction secondary-index DDL enforcement/drop refresh, primary-key idempotent ADD no-op preservation, primary-key, descending-primary-key, and composite direction primary-key replacement DDL refresh, foreign-key ALTER add/drop refresh, foreign-key referential actions, composite foreign-key coverage, deep foreign-key cascade-chain update/delete coverage, generated-column foreign-key coverage including virtual generated child and rejected action policy, cyclic foreign-key coverage including three-table cascade and set-null variants, same-schema foreign-key parent-table/child-table rename refresh, cross-schema foreign-key parent-table/child-table rename refresh, same-schema and cross-schema foreign-key multi-pair parent/child rename refresh, CHECK constraint ALTER add/drop enforcement, generated-column ALTER add/drop and same-kind expression-replacement refresh, table charset-conversion DDL refresh, row-format DDL refresh, table-comment DDL refresh, force-rebuild DDL refresh, column-default SET/DROP refresh, column idempotent ADD/DROP refresh, column add-modify-rename-drop ALTERs, explicit instant ADD/DROP/reorder column metadata, and instant FIRST/AFTER stored-column placement including `LOCK=DEFAULT`, `LOCK=SHARED`, and `LOCK=EXCLUSIVE` variants, column rename, and virtual generated-column add/drop including `LOCK=SHARED`/`LOCK=EXCLUSIVE` variants refresh, schema create/drop lifecycle refresh, schema default charset/collation DDL refresh plus schema-default rewrite crash recovery, and no-live absent-schema reopen checks, cross-schema table rename with `.frm`/`.ibd` movement, same-schema and cross-schema multi-pair rename-cycle tablespace swaps, and ownerless/native reopen before and after forced `.shm` rebuild, view create/query/drop, replacement/alter, idempotent create/drop, column-list, check-option, prepared check-option DML, direct/prepared non-updatable view diagnostics, invalid view dependency diagnostics, nested check-option, and security/definer metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, trigger create/fire/drop, replacement/update/delete, ordering/show-create, and idempotent create/drop metadata refresh with ownerless/native reopen before and after forced `.shm` rebuild, no-live ownerless/native exclusive reopen of the broader DDL final state before and after forced `.shm` rebuild, local DDL followed by dictionary/table flush, foreign keys, CHECK constraints, generated columns, online/in-place index alter, concurrent same-named InnoDB temporary tables, killed temporary-table peer cleanup while another temp-table peer remains live, ownerless InnoDB-only engine policy, ownerless stored-routine DDL including package specification/body and routine-execution policy rejection, ownerless sequence SQL policy rejection, ownerless table-admin SQL policy rejection, ownerless `LOCK TABLES` SQL policy rejection, ownerless `FLUSH TABLES ... WITH READ LOCK`/`FOR EXPORT` SQL policy rejection while plain ownerless `FLUSH TABLES` remains covered, ownerless table-directory SQL policy rejection, ownerless top-level, ALTER, inline, and idempotent `FULLTEXT`/`SPATIAL` index DDL policy rejection, ownerless partitioned table DDL policy rejection, and a killed uncommitted writer whose state blocks peer cleanup until a no-live-process reopen rebuilds volatile coordination. The opt-in `ownerless-stress` preset reruns the independent-table reader/writer stress case at 200 writer iterations and 400 reader polls, with deterministic independent-table stress SQL trace export covering the same per-table writer schedule and aggregate reader oracle for external harness input. It runs concurrent DDL workers with live DML readers/writers plus forced `.shm` rebuild and native exclusive reopen checks, and deterministic DDL stress SQL trace export covers the same create/alter/index/rename/truncate/drop plus DML schedule for external harness input, deterministic DDL lifecycle SQL trace export covers create/rename/truncate/force/drop/recreate final-state oracles for external harness input, runs same-name temporary-table churn across ownerless processes plus forced `.shm` rebuild and native exclusive reopen checks for the resulting permanent table, and deterministic temporary-table stress SQL trace export covers the same session-local temporary-table churn plus post-worker permanent-table oracle for external harness input, runs explicit multi-statement transaction/savepoint stress with deterministic aggregate checks, forced `.shm` rebuild checks, native exclusive reopen checks, and deterministic transaction stress SQL trace export for external harness input, runs shared-table checksum stress with mixed direct/prepared DML writers, bounded retry on ownerless statement-lock busy plus MariaDB 1205/1213 contention, deterministic sum/version/weighted-sum oracle checks, forced `.shm` rebuild checks, native exclusive reopen checks, and deterministic checksum stress SQL trace export for external harness input, runs pseudo-random shared-table transaction stress with savepoint rollback, full rollback, live aggregate-reader bounds, bounded retry on 1205/1213, deterministic final oracles, forced `.shm` rebuild checks, and native exclusive reopen checks, and runs foreign-key graph stress with concurrent ownerless workers over `CASCADE`, `SET NULL`, and `RESTRICT` edges, bounded retry on 1205/1213, tracked transaction page LSN coverage, DML current-read refresh, deterministic aggregate/referential oracles, forced `.shm` rebuild checks, and native exclusive reopen checks, each with a 900-second timeout; every stress case also asserts final no-peer page-version WAL reclamation. The `ownerless-test-hooks` preset adds unsafe deterministic transaction-registration, page-version before-append and after-append publish/checkpoint, page-visible publish-before-checkpoint, page-visible-checkpoint, redo-reservation, redo-gap writer blocking, redo completed-write, redo latest-before-checkpoint, redo latest-after-checkpoint, native checkpoint reclamation crash/race with resumed-closer WAL non-growth after newer peer commits, primitive active-pin page-version boundary reclamation, consistent-snapshot pre-execution pin/race coverage, dictionary-DDL begin/before-finish/after-finish crash injection, unique-index replacement crash recovery, same-schema, cross-schema, and same-schema multi-pair `RENAME TABLE` crash-at-dictionary-before-finish file-move recovery, view replacement/alter, column-list, check-option, nested check-option, and security crash recovery, and negative-proof coverage |
| Ownerless DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills same-schema, cross-schema, and same-schema multi-pair swap `RENAME TABLE` writers after native file movement, standalone `CREATE INDEX` and `DROP INDEX` writers after native secondary-index metadata creation/removal, duplicate top-level `CREATE INDEX IF NOT EXISTS`, missing top-level `DROP INDEX IF EXISTS`, duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, missing `ALTER TABLE ... DROP INDEX IF EXISTS`, duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS`, and duplicate `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers after MariaDB success, a `CREATE OR REPLACE UNIQUE INDEX` writer after native replacement metadata, an `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY` writer after native primary-key replacement, duplicate `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` no-op writer after MariaDB success, `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` and `ALTER TABLE ... DROP FOREIGN KEY` writers after native foreign-key metadata creation/removal, `ALTER TABLE ... ADD CONSTRAINT ... CHECK`, `ALTER TABLE ... DROP CONSTRAINT` for CHECK constraints, plus `ALTER TABLE ... ADD COLUMN`, `ALTER TABLE ... DROP COLUMN`, `ALTER TABLE ... MODIFY COLUMN`, `ALTER TABLE ... RENAME COLUMN`, duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`, missing `ALTER TABLE ... DROP COLUMN IF EXISTS`, missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS`, missing `ALTER TABLE ... RENAME COLUMN IF EXISTS`, missing `ALTER TABLE ... CHANGE COLUMN IF EXISTS`, missing `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and missing `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` writers after native table-definition mutation or MariaDB no-op success, including missing rename/change/default no-ops over generated-column/CHECK expression metadata, simple `CREATE VIEW` and `DROP VIEW` writers after native view definition-file creation/removal, `CREATE OR REPLACE VIEW` and `ALTER VIEW` writers after native view definition rewrite, explicit column-list create/replace/alter writers after native alias metadata storage/rewrite, check-option create/replacement/alter writers after native check-option metadata storage/rewrite, nested check-option outer-replacement and inner-alter writers after native nested view definition rewrite, explicit definer create and invoker replacement view writers after native security metadata storage/rewrite, duplicate `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op writers after MariaDB success, an `ALTER TABLE ... FORCE, ALGORITHM=COPY` writer after native table-copy rebuild, an `ALTER TABLE ... CONVERT TO CHARACTER SET` writer after native charset-conversion metadata/storage update, `ALTER TABLE ... ROW_FORMAT=DYNAMIC`, `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`, `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`, and `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16` writers after native row-format rebuilds, an `ALTER TABLE ... COMMENT` writer after native table-comment metadata update, an `ALTER TABLE ... AUTO_INCREMENT` writer after native high-watermark persistence, an `ALTER COLUMN ... SET DEFAULT` writer after native default metadata update, a `TRUNCATE TABLE` writer after native truncate/recreate, a `DROP TABLE` writer after native file removal, a `DROP DATABASE` writer after native schema/table removal, an `ALTER DATABASE` writer after native `db.opt` rewrite, and duplicate `CREATE DATABASE IF NOT EXISTS` plus missing `DROP SCHEMA IF EXISTS` no-op writers after MariaDB success, but before ownerless dictionary finish; it verifies live-peer cleanup remains busy until no-live recovery, verifies recovered present/absent secondary-index metadata, forced-index reads for create, forced-index rejection for drop, recovered idempotent secondary-index no-op behavior with original key-part preservation, missing-index absence, errno 1061 for plain duplicate create, post-recovery writes, and forced-index reads from top-level and ALTER-table no-op branches, recovered unique-index idempotent no-op behavior with original unique-key preservation, attempted-key non-enforcement, errno 1061 for plain duplicate create or ALTER add, and duplicate-key enforcement, recovered replacement unique-index metadata and duplicate-key enforcement, recovered primary-key replacement metadata with duplicate-key enforcement on the replacement key and duplicate values allowed on the former key, recovered primary-key idempotent no-op behavior with original-key preservation and candidate-key non-uniqueness, recovered added foreign-key metadata with orphan-row rejection, rejected-row absence before later commit, and valid child writes, recovered dropped foreign-key metadata absence with orphan-row writes and parent deletes allowed, recovered generated-column FK ADD metadata/enforcement and generated-column FK DROP metadata absence with orphan writes and parent deletes allowed, recovered CHECK metadata with errno 4025 enforcement, recovered dropped-CHECK metadata absence with formerly invalid writes allowed, recovered present/absent view metadata, `.frm` file state, view query behavior, replacement/altered view column metadata, explicit column-list alias metadata and ordinal positions, stale alias rejection, recovered check-option and nested check-option metadata, updatability metadata, errno 1369 invalid-DML enforcement, old exposed-column rejection, recovered security type and non-empty definer metadata, original view-definition preservation, missing-view absence, and base-table writes, recovered added-column metadata/default values, recovered idempotent column no-op behavior with original default preservation, missing-column absence, errno 1060 for plain duplicate add, errno 1091 for plain missing drop, missing modify/rename/change/default no-op preservation with errno 1054 for plain retries, expression-table missing rename/change/default preservation with generated-column values, real defaults, and CHECK enforcement unchanged, and post-recovery writes, recovered column-default metadata and post-recovery default-backed insert behavior, recovered charset/collation metadata and retained/post-recovery rows, recovered table-comment metadata and retained/post-recovery rows, absent dropped-column metadata, recovered modified-column width/default metadata and widened-value writes, recovered renamed-column metadata with old-name rejection and new-name writes, recovered schema default metadata with pre-alter table collation preservation and post-recovery default inheritance, recovered schema idempotent no-op behavior with original defaults, real schema/table preservation, missing-schema absence, and errno 1007 for plain duplicate create, recovered AUTO_INCREMENT high-watermark metadata with monotonic implicit ID allocation, recovered generated-column and CHECK expression behavior after a dependent column rename, recovered force-rebuilt InnoDB table/space/index metadata, copied payload bytes, recovered dynamic and compressed row-format metadata, retained row payloads, compressed 4 KiB, 8 KiB, and 16 KiB ZBLOB page evidence, and post-recovery writes, verifies ownerless reopen before and after forced `.shm` rebuild, and verifies native exclusive reopen of the recovered table, column, index, constraint, view, or schema state after rebuild |
| Ownerless cross-schema multi-rename DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills a three-pair cross-schema `RENAME TABLE` swap writer after native InnoDB file movement but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, the final schema/table names and `.frm`/`.ibd` files are present, the temporary table name and InnoDB dictionary entry are absent, InnoDB `SPACE` identities are swapped with the table names, post-recovery writes succeed, and ownerless/native reopen before and after forced `.shm` rebuild observe the same state |
| Ownerless table-copy DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills representative `CREATE TABLE ... LIKE` and `CREATE TABLE ... SELECT` writers after native destination table creation, including CTAS row population, but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered `.frm`/`.ibd` files, `INFORMATION_SCHEMA.TABLES` and column metadata, copied secondary-index metadata for `LIKE`, CTAS copied rows, post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless table-replacement DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills representative `CREATE OR REPLACE TABLE`, `CREATE OR REPLACE TABLE ... LIKE`, and `CREATE OR REPLACE TABLE ... AS SELECT` writers after native old-table replacement or replacement-copy completion but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered replacement `.frm`/`.ibd` files, old-column and old-index absence, new-column and new-index metadata, empty replacement rowset for ordinary and LIKE replacement, copied secondary-index metadata for LIKE, CTAS copied rows, post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless table-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE TABLE IF NOT EXISTS` and missing `DROP TABLE IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original table definition and keeps plain duplicate create returning errno 1050, missing drop preserves the real table while keeping the missing table metadata/files absent, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless schema-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE DATABASE IF NOT EXISTS` and missing `DROP SCHEMA IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original schema defaults and inherited column collation while plain duplicate create returns errno 1007, missing drop preserves the real schema/table while keeping the missing schema metadata/directory absent, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless index-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate top-level `CREATE INDEX IF NOT EXISTS` and missing top-level `DROP INDEX IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original key part and keeps plain duplicate create returning errno 1061, missing drop preserves the real index while keeping the missing index metadata absent, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless ALTER index-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` and missing `ALTER TABLE ... DROP INDEX IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original key part and keeps plain duplicate ALTER-add returning errno 1061, missing drop preserves the real index while keeping the missing index metadata absent, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless unique-index idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create/add preserves the original unique key and keeps plain duplicate create or ALTER-add returning errno 1061, the attempted replacement key remains non-enforced, duplicate-key enforcement for the original unique key remains active, post-recovery writes and forced-index reads succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless unique-index drop DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills a `DROP INDEX` writer for an active unique secondary index after native metadata removal but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, the unique-index metadata is absent, forced-index reads on the dropped name fail, the formerly duplicate key shape inserts successfully after recovery, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless primary-key DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills plain `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)` and composite direction `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)` writers after native clustered-key rebuild but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, recovered primary-key metadata and key-part direction, old-key absence from `PRIMARY`, duplicate-key enforcement on the replacement key, old-key duplicate writes where applicable, forced-index reads, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless view-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate create preserves the original view definition and keeps plain duplicate create returning errno 1050, missing drop preserves the real view while keeping the missing view metadata/files absent, post-recovery base-table writes remain visible through the view, and ownerless/native reopen plus forced `.shm` rebuild observe the same state; both selectors are also registered as standalone hook CTests for visible CI timing and failure attribution |
| Ownerless primary-key idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original `PRIMARY(id)` clustered key and keeps plain duplicate primary-key add returning errno 1068, the attempted candidate `code` key remains non-unique, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless column-idempotent DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills duplicate `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing `ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, duplicate add preserves the original column and default while plain duplicate add returns errno 1060, missing drop preserves the real column while keeping the missing column absent and plain missing drop returning errno 1091, post-recovery writes succeed, and ownerless/native reopen plus forced `.shm` rebuild observe the same state |
| Ownerless column IF EXISTS DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills missing `ALTER TABLE ... MODIFY COLUMN IF EXISTS`, `ALTER TABLE ... RENAME COLUMN IF EXISTS`, `ALTER TABLE ... CHANGE COLUMN IF EXISTS`, `ALTER TABLE ... ALTER COLUMN IF EXISTS SET DEFAULT`, and `ALTER TABLE ... ALTER COLUMN IF EXISTS DROP DEFAULT` no-op writers after MariaDB returns success but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, these paths preserve the original real column metadata/default, keep the missing and attempted renamed/changed columns absent, keep plain missing modify/rename/change/default retries returning errno 1054, allow post-recovery writes, and ownerless/native reopen plus forced `.shm` rebuild observe the same state; missing `RENAME COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`, and default-alter variants are also covered on generated-column/CHECK expression tables, proving the real column, stored and virtual generated expressions, real-column defaults, CHECK enforcement, and missing attempted names survive recovery; external randomized DDL oracle execution remains planned |
| Ownerless index metadata crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset also kills `CREATE OR REPLACE UNIQUE INDEX`, unique-secondary `DROP INDEX`, `ALTER TABLE ... RENAME INDEX`, and `ALTER TABLE ... ALTER INDEX ... IGNORED`/`NOT IGNORED` writers after native index metadata changes but before ownerless dictionary finish; recovery verifies live-peer cleanup remains busy until no-live recovery, old/new index-name metadata, replacement unique-key metadata/enforcement, dropped unique-index metadata absence and duplicate-key release, ignored/not-ignored metadata, final forced-index reads, later writes, ownerless/native reopen, and forced `.shm` rebuild |
| Ownerless trigger DDL crash coverage | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset kills simple `CREATE TRIGGER` and `DROP TRIGGER`, `CREATE OR REPLACE TRIGGER`, ordered `CREATE TRIGGER ... PRECEDES ...`, duplicate `CREATE TRIGGER IF NOT EXISTS`, missing `DROP TRIGGER IF EXISTS`, delayed missing-dependency `CREATE TRIGGER`, explicit `CREATE DEFINER=CURRENT_USER TRIGGER`, and stored-function-body `CREATE TRIGGER` writers after native `.TRG`/`.TRN` metadata creation/removal, rewrite, no-op preservation, delayed dependency acceptance, or definer metadata storage but before ownerless dictionary finish; it verifies live-peer cleanup remains busy until no-live recovery, recovered present/absent `INFORMATION_SCHEMA.TRIGGERS` metadata including non-empty definer metadata for the explicit-definer case, recovered trigger-file presence/absence, trigger firing after recovered create, replacement, idempotent no-op preservation, delayed dependency creation, definer recovery, stored-function trigger metadata recovery, ownerless stored-routine execution rejection before base-row mutation, and ordinary native stored-function trigger firing, recovered MariaDB 1146 failure when the missing dependency is absent, recovered `ACTION_ORDER`/firing order after ordered create, dropped-trigger non-firing after recovered drop, absent missing-trigger `.TRN` state after recovered missing drop, `SHOW CREATE TRIGGER` for recovered present triggers including explicit `DEFINER=` metadata and rejection for the dropped trigger, ownerless reopen before and after forced `.shm` rebuild, and native exclusive reopen after rebuild, with the explicit-definer selector also registered as a standalone hook CTest for visible CI timing and failure attribution, while broader privilege/security and randomized trigger crash variants remain planned |
| Ownerless online DDL option matrix | 🟡&nbsp;Partial | Focused ownerless SQL coverage verifies already-open peer refresh, final ownerless/native reopen, and forced `.shm` rebuild for accepted ordinary secondary-index `ALGORITHM=NOCOPY, LOCK=SHARED`, `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`, `ALGORITHM=INPLACE, LOCK=NONE`, `ALGORITHM=INPLACE, LOCK=SHARED`, and `ALGORITHM=INPLACE, LOCK=EXCLUSIVE` add/drop combinations plus a unique secondary-index `ALGORITHM=INPLACE, LOCK=SHARED` add/drop pair with duplicate-key enforcement, extending the existing `NOCOPY`, `INPLACE`, `INSTANT`, and `COPY` option coverage while broader randomized DDL oracle execution remains planned |
| Ownerless READ UNCOMMITTED policy | 🟡&nbsp;Partial | Ownerless read/write opens reject `SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED`, session-scoped READ UNCOMMITTED changes, all `tx_isolation`/`transaction_isolation` assignments, and `SET STATEMENT` isolation overrides before entering unproven cross-process dirty-read semantics or untracked isolation-variable state; ordinary exclusive embedded opens still inherit MariaDB/InnoDB `READ UNCOMMITTED` behavior |
| Ownerless checkpoint hot-path publication | 🟡&nbsp;Partial | Hook-driven ownerless raw-latest and page-visible checkpoint writes now snapshot shared redo state while holding the checkpoint byte-range lock and write the monotonic latest/visible pair without rereading the `.ckpt` legacy payload; startup baseline seeding and no-live native checkpoint promotion still use the file-read merge path, durable sync ordering remains unchanged, `.ckpt` latest/visible records have checksummed generation slots with legacy fallback, repeated same-pair checkpoint publications elide record rewrites for non-durable updates and for durable updates only after the same process has already synced the same pair on the same checkpoint fd, advancing updates skip rewriting the legacy latest/visible payload once a valid generation record exists, single-owner hook writes can reuse a process-local generation-record cache while the process registry proves no peer owner has joined since registration, capped visible-fast append-batched statements preserve the first latest-only checkpoint update and coalesce later non-durable latest-only updates in the same statement for implicit/autocommit writes and proven explicit-transaction insert writes while savepoint-disqualified explicit transaction writes remain conservative, focused SQL coverage verifies generation stability, later advancement, legacy-write elision, single-owner generation-cache hits, deferred-latest coalescing, forced-`.shm` rebuild recovery, and the performance counters, hook-build crash coverage kills writers before and after raw-latest checkpoint persistence and before and after page-visible checkpoint persistence, verifies live-peer cleanup and normal plus forced-`.shm` rebuild recovery, while cross-process group commit and broader lazy/batched checkpoint writes remain planned |
| Ownerless tableless SELECT fast path | 🟡&nbsp;Partial | Non-locking direct and prepared ownerless `SELECT`/`WITH` statements with no `FROM` or `JOIN` token now skip ownerless page-version reads and global ownerless page refresh, so common probe queries such as `SELECT 1` do not open a baseline page-version pin or enable page visibility while a prepared cursor is active; focused SQL coverage verifies direct and prepared `SELECT 1` leave `mylite_ownerless_pressure_status()` with zero active page-version pins, while real table reads, CTEs/subqueries with table references, locking reads, writes, DDL, and committed peer-update visibility remain on the existing ownerless refresh path. In a reduced production probe, ownerless/ordinary direct `SELECT 1` throughput improved from the prior `0.6658` sample ratio to `0.7871`, and prepared `SELECT 1` improved from `0.6577` to `0.9292`; the stats-enabled production probe now also emits direct/prepared tableless and InnoDB point-select read-stage attribution, with a reduced 25-select tableless sample showing zero page-version read hooks and native MariaDB execute time dominating tableless read cost, while write-path page-publication gaps remain separate |
| Ownerless read hook attribution | 🟡&nbsp;Partial | The stats-enabled production embedded performance probe now splits ownerless MDL, transaction, and read-view hook counts and elapsed time for tableless and InnoDB point-select read windows, allowing the real-table read slowdown to be separated from page-version read hooks, WAL scans, and statement-boundary refresh. A reduced 100-select attribution sample reported zero hooks for tableless reads and about one MDL acquire/release, one transaction snapshot, and one read-view register/deregister per point select, with only about `0.008-0.011 ms/select` in those hook callback bodies. This is diagnostic instrumentation only; it does not skip MDL/read-view publication or change snapshot, DDL, active-reader, or directory-lifecycle correctness policy |
| Ownerless refresh attribution | 🟡&nbsp;Partial | The stats-enabled production embedded performance probe now splits ownerless statement-boundary refresh into dictionary check, shared redo/process/transaction snapshot, active-pin snapshot, baseline pin, transaction-horizon advance, current-read-view close, native flush, external refresh, handle pin, clean-page refresh, visibility push/enable, and local-native-current-read decision counters. A reduced 100-select attribution sample showed tableless reads doing none of the expensive refresh work, while direct/prepared point selects used the local-native-current-read path and spent almost all refresh time in the shared snapshot (`0.034-0.035 ms/select`). This is diagnostic instrumentation only; it does not change dictionary, page-version pin, native visibility, DDL, active-reader, or peer join/leave policy |
| Ownerless single-owner refresh snapshot | 🟡&nbsp;Partial | Ownerless statement refresh now uses process active count and process generation to infer no other live ownerless process exists in the current single-owner epoch, avoiding repeated process-registry explicit-transaction scans and transaction-registry owner-active scans while preserving the existing conservative helper paths whenever the single-owner epoch is not proven. A reduced stats-enabled point-select sample moved refresh shared-snapshot time from `0.034-0.035 ms/select` to `0.001 ms/select`, and a 1000-select stats-off sample reported direct point-select ratio `0.9028` and prepared point-select ratio `0.7707`; peer-present refresh, page-version pins, DDL, active-reader, and generation-change behavior stay on the existing paths |
| Ownerless page-log visible-fast append batching | 🟡&nbsp;Partial | Pure `INSERT ... VALUES` visible-fast-path ownerless statements with one through 32768 streaming-proven row constructors now keep one page-log append session across the statement's ownerless mini-transactions, coalesce repeated user data/index page images through the existing transaction-deferred page publication proof only while the process registry proves a single-owner epoch, and release/sync before page-visible LSN publication; peer-present ownerless statements keep immediate page publication, and focused SQL coverage verifies page-version records still publish, native history WAL proof remains active, page-log append session begin/end counts collapse for single-row and capped multi-row visible-fast inserts, 32768-row single-owner statements use transaction-deferred image/buffer publication without snapshot-boundary immediate user-page publication, the 32769-row boundary stays outside append batching and deferred latest-checkpoint coalescing, deferred redo completion now batches up to 61 ranges while preserving an active-owner entry plus two peer headroom slots in the 64-slot active-reservation table, peer-open truncate/allocation coverage keeps large single-row inserts safe, and unsupported upsert branches keep conservative flush behavior after a previous unbounded row-list attempt regressed bulk timing, while row lists above 32768, broader DML/DDL, native row/undo reductions, and group-commit batching remain planned |
| Ownerless default-checked bulk insert | 🟡&nbsp;Partial | Ownerless single-owner autocommit direct `INSERT ... VALUES` row lists into an empty InnoDB table with exactly one clustered index and no foreign-key relationships may now use MariaDB's existing `TRX_UNDO_EMPTY` bulk-buffer path even when SQL `unique_checks` and `foreign_key_checks` remain at their defaults; focused coverage asserts the new ownerless-only default-checked bulk-start counter is positive, duplicate-key row-list failure leaves no partial rows, a later valid row list succeeds, CTAS and `INSERT ... SELECT` copy rows without using that counter, peer-present cross-process SQL does not use the counter, and visible-fast/page-version/history-proof/native-support publication remains active. Ordinary non-ownerless opens keep upstream MariaDB's stricter check-disabled bulk requirement, and secondary indexes, foreign-key tables, peer-present cross-process statements, explicit transactions, duplicate-handling DML, `INSERT ... SELECT`, DDL, and broader SQL bulk coverage remain planned |
| Ownerless single-owner record-wait skip | 🟡&nbsp;Partial | Ownerless insert-intention record wait-until callbacks now skip the shared record-lock registry availability probe only when the process registry proves a continuous single-owner epoch (`active_count == 1` and registry generation equals the current owner generation) and the record-lock registry proof finds no waiting entries or foreign active record owner. MariaDB's local record-lock conflict check still runs before the callback, and live-peer, stale-generation, unmapped, foreign-lock, and error cases keep the shared-registry path. Focused SQL coverage asserts positive wait-until calls and all skipped calls in the single-owner multi-row insert path, product-hook coverage proves a same-process synthetic external ownerless record lock still becomes a visible shared wait, and peer-history coverage proves zero allowed skips plus a positive generation-block counter after a peer joins and leaves; the production performance probe emits compact and raw skip counters, while broader multi-peer rollback/redo/checkpoint and DDL recovery work remains planned |
| Ownerless page-log checksum handoff | 🟡&nbsp;Partial | Ownerless InnoDB page publish now precomputes the MyLite page-log full-page checksum after a page passes publish checks and before the page-log append path, then passes that checksum through direct, external-snapshot-lineage, and append-session page-log append variants; the durable WAL checksum field and recovery validation are unchanged, primitive coverage proves checksum-handoff append readback and production performance output reports precomputed-checksum records plus moved publish-hook checksum time, while the optimization moves the checksum scan out of the append path rather than eliminating the scan entirely |
| Ownerless MTR publish-list scan elision | 🟡&nbsp;Partial | The made-dirty ownerless mini-transaction commit path now collects modified page pointers during MariaDB's existing flush-list pass and publishes that collected list after commit-log/ownerless-redo latch release, preserving page-version volume, publication ordering, page latch lifetime, and native-support/history-proof semantics while avoiding the second full MTR memo scan used only to rediscover modified pages; a reduced production attribution sample kept `3.000` MTR-published pages and `3.020` page-log appends per ownerless autocommit insert while moving `page_write_publish_scan_calls_per_insert` and `page_write_publish_scan_ms_per_insert` from the prior `1.275` / `0.049` sample to `0.000` / `0.000`, with larger native commit, history-proof publication, and page-log append costs still remaining |
| Ownerless MTR release memo prechecks | 🟡&nbsp;Partial | Generic `mtr_t::release()` and `release_unlogged()` now use the existing MTR page-write vector membership guard before entering the ownerless page-write leave helper, matching the no-dirty commit-log release loop's skip for non-member page memo slots while preserving member release order before native latch unlock; focused visible-fast, history-proof, native-support, FK-cache, commit-race, active-reader, and stress selectors passed, and a reduced four-row-bulk production probe kept page-version/native-support/page-log/commit-visibility counts stable while reducing bulk leave calls, but release-memo/no-dirty-loop timing remained noisy and native-support/history-proof publication volume plus redo/checkpoint reconciliation remain planned |
| Ownerless transaction-deferred MTR vector elision | 🟡&nbsp;Partial | Transaction-deferred ownerless data-page write locks are now recorded in the transaction page-write registry without also being inserted into the mini-transaction page-write vector that is only needed for MTR-scoped release; focused multi-row visible-fast coverage asserts transaction-deferred page publication and the elision counter remain active, while MTR-scoped native-support/history-proof pages keep the existing release helper path. A reduced 100-row bulk production probe preserved `2.000` page versions, `2.000` native-support published pages, `100.000` native-support elided pages, fast commit visibility, and zero visibility flushes per statement while moving no-dirty page-leave time from the earlier `0.477 ms/statement` sample to `0.187 ms/statement`; native-support/history-proof proof volume, redo/checkpoint reconciliation, and broader SQL-level concurrency coverage remain planned |
| Ownerless inline MTR page tracking | 🟡&nbsp;Partial | MTR-scoped ownerless page-write ownership now keeps the first tracked page identity inline in `mtr_t` and allocates the existing overflow vector only for a second distinct page, preserving transaction-deferred elision, member release ordering, page-version publication, native-support/history-proof behavior, and native latch release order. The production performance probe reports inline first-page records, overflow allocations, overflow inserts, duplicate inline hits, and inline promotions; a reduced 100-row bulk attribution sample preserved `2.000` page versions, `4.500` page-log appends, `2.000` native-support published pages, fast commit visibility, and `180.500` deferred latest-checkpoint coalesces per statement while recording `91.200` inline first-page records and only `1.000` overflow vector allocation per statement, with ownerless `mysql_query()` at `3.815 ms/statement`. Stats-off throughput remained noisy, so this is a bounded allocation-path reduction rather than completion of the broader write-path performance gap |
| Ownerless page-type undo classification | 🟡&nbsp;Partial | Ownerless page-write classification now checks MariaDB's definite user index/BLOB page types before using the path-based undo-tablespace fallback, so hot `FIL_PAGE_INDEX`, `FIL_PAGE_RTREE`, `FIL_PAGE_TYPE_BLOB`, `FIL_PAGE_TYPE_ZBLOB`, and `FIL_PAGE_TYPE_ZBLOB2` pages avoid repeated `fil_space_t::get()` path inspection while known undo space ids, `FIL_PAGE_UNDO_LOG`, and ambiguous metadata pages keep the conservative undo-space path. A reduced stats-enabled 100-row bulk attribution sample reported ownerless bulk `mysql_query()` at `3.918 ms/statement`, no-dirty page publication at `0.126 ms/statement`, and no-dirty page-write leave at `0.164 ms/statement`; stats-off 100-row bulk ratios remained noisy at `0.2179` and `0.2590`, so the remaining write-path performance gap, especially about `162` redo-leave hook calls per 100-row statement, remains planned |
| Ownerless no-dirty commit-loop attribution | 🟡&nbsp;Partial | The production embedded performance probe now splits the ownerless no-dirty MTR commit-log loop into space-write leave, page publication, page-write leave, and native page-unlock buckets, plus compact autocommit and bulk insert summary rows, so the remaining ownerless write-path gap can be assigned before changing MTR release or publication semantics; three reduced stats-enabled bulk insert samples reported `0.049`-`0.070 ms/statement` in the no-dirty loop, with page publication accounting for `0.039`-`0.055 ms/statement`, page-write leave for `0.008`-`0.013 ms/statement`, and effectively zero space-leave/page-unlock time, pointing the next write-path slice at page publication rather than native latch unlock; this is diagnostic instrumentation only and does not change page-write ownership, page publication order, redo/checkpoint behavior, WAL format, or SQL behavior |
| Ownerless bulk page-write phase split | 🟡&nbsp;Partial | The stats-enabled production embedded performance probe now snapshots the first ownerless bulk row-list statement for existing page-publish, database hook, page-write, page-log append, and commit-visibility counters and emits `first_` and `remaining_` page-write phase summaries without changing aggregate metric names; a reduced two-statement 100-row probe reported the later non-empty-table statement at `0.438 ms` page-write commit-log time, including `0.105 ms` redo leave, `0.178 ms` commit-log publish, `0.244 ms` no-dirty loop, `0.157 ms` no-dirty page-publish, `0.010 ms` page-leave, and `0.011 ms` page-unlock time per statement, while a one-statement guard emitted `0.000` remaining phase averages. This is diagnostic instrumentation only; it does not change page-write ownership, WAL format, redo/checkpoint behavior, native undo, or SQL behavior |
| Ownerless held native-support publish skip | 🟡&nbsp;Partial | Pages already recorded in the transaction-local held native-support page-write list can now skip the redundant ownerless publish helper dispatch when page-publish diagnostics are disabled and the page has no active rollback-segment or undo history-proof role; page-write perf stats expose the new `native_support_transaction_publish_skipped` counter without enabling page-publish stats. Focused native-support WAL-elision SQL coverage proves detailed page-publish diagnostics still use the full helper with zero held-publish skips, while a reduced 1000-row page-write-only bulk probe reported `20` held native-support locks, `1822` already-held hits, and `901` held-publish skips. This does not change page-write lock lifetime, native undo, history-proof WAL, redo/checkpoint ordering, page-version WAL format, SQL behavior, or the remaining broader native recovery gaps |
| Ownerless page-write release tracking elision | 🟡&nbsp;Partial | Ownerless SQL wrappers still keep fallback page-write lock release for statement paths that do not pass through normal native commit cleanup, but successful native bulk page-write release now clears the matching SQL-layer tracked transaction id during the active statement so the wrapper's boundary cleanup takes the empty fast path instead of redundantly taking the shared page-write registry latch and scanning already-released records; the production embedded performance probe now reports tracked-release calls, time, empty fast-path calls, fallback transaction ids, and native-cleared ids so future ownerless write-path regressions can distinguish cleanup bookkeeping from WAL/page-publication overhead |
| Ownerless page-write timeout retry | 🟡&nbsp;Partial | Ownerless MTR and buffer pre-read page-write lock timeouts are now treated as internal physical-page contention that refreshes and retries instead of poisoning InnoDB transaction error state or returning null for mandatory dictionary pages; amplified DDL stress no longer hits the stale `DB_LOCK_WAIT_TIMEOUT` transaction-start assertion after `TRUNCATE`, amplified same-name temporary-table stress no longer segfaults in `dict_hdr_get_new_id()`, and SQL-visible retained-WAL pressure plus statement-lock busy behavior remains owned by the MyLite statement policy layer |
| Ownerless append-only attribution probe | 🟡&nbsp;Partial | The production embedded performance probe now supports `MYLITE_PERF_OWNERLESS_APPEND_STATS=1`, which enables ownerless database, page-log append, page-log scan, page-log sync, and bulk `mylite_exec()` counters without enabling page-publish stats. CI publishes this as `ownerless-append-attribution.log` alongside the existing page-publish attribution report, and also publishes a stats-enabled 100-row bulk `large-row-ownerless-attribution.log`, so history-proof pair calls, append timing, page-publish, redo-leave, no-dirty-loop, and bulk `mysql_query()` attribution are measured on the production fast path where `mtr_t::ownerless_history_proof_publish_pair()` remains active. This is diagnostic instrumentation only; it does not change SQL behavior, WAL format, native redo/checkpoint policy, or the remaining ownerless concurrency gaps |
| Ownerless platform probe device cache | 🟡&nbsp;Partial | Successful ownerless directory platform probes are now remembered per `st_dev` inside the current process. A later fresh MyLite directory on the same device may skip the child-process MAP_SHARED/byte-range-lock/wait-backend probe, but still creates its own `concurrency/` directory and writes its own `mylite-ownerless-platform.meta` proof before ownerless mode is accepted. Unsafe forced probe-failure hooks bypass the process cache so negative coverage still exercises the real probe path. A reduced production probe reported first-directory platform probing at `211.855 ms` and second same-device fresh-directory probing at `0.047 ms`; this removes repeated probe work inside one process, not cold MariaDB/InnoDB startup |
| Ownerless explicit transaction publish attribution | 🟡&nbsp;Partial | The production embedded performance probe records compact stats-enabled summaries for prepared single-row inserts inside one explicit ownerless transaction, including page versions per insert and transaction, native-support published/elided page classes, page-log append session counts and timing, deferred latest-checkpoint coalescing, page-write commit-log timing, final commit-visibility timing, transaction-deferred page publication, undo-report MTR cost, undo-cache reuse, write-history flush cost, and ownerless-minus-ordinary row-insert/undo deltas; the latest reduced probe sample moved the proven explicit-transaction page-log appends from 10 direct plus 2 session appends to 0 direct plus 12 session appends, and append batching remains scoped to proven statements rather than holding the page-log append lock across user transaction work |
| Ownerless explicit transaction history proof | 🟡&nbsp;Partial | Explicit ownerless transactions now carry a conservative per-handle proof from eligible `INSERT ... VALUES` writes to the later `COMMIT`, allowing prepared explicit insert transactions to publish committed ownerless visibility without the later unproven-statement dirty-page flush fallback, elide pre-commit rollback-segment `FIL_PAGE_UNDO_LOG` native-support page-version WAL in the transaction's own rollback-segment space, use the existing rollback-segment/undo history WAL proof instead of the ownerless write-history page flush in `trx_t::write_serialisation_history()`, batch COMMIT-time transaction-deferred, history-proof, native-support, and external-snapshot-lineage page-log appends for the proven COMMIT statement, and coalesce later non-durable latest-only checkpoint updates inside each proven insert statement after preserving the first update. Focused SQL coverage uses prepared inserts in one explicit transaction and verifies fast commit publication, zero conservative flush/unproven counters, zero write-history ownerless flush pages, positive rollback-segment and undo history-proof page publication matching sample counters, transaction page publication at commit, all proven-path page-log appends flowing through append sessions, native-support undo elision, deferred latest-checkpoint coalescing, no publish failure, same-handle visibility, ownerless reopen, forced-`.shm` native reopen visibility, and a savepoint-controlled transaction that deliberately stays on the conservative unproven path while preserving only the pre-savepoint row and not coalescing after disqualification; `INSERT ... SELECT`, `UPDATE`, `DELETE`, `REPLACE`, DDL, locking-read transactions, savepoint-controlled transactions beyond this negative proof, foreign-key target inserts, and broader native redo/checkpoint reconciliation remain planned |
| Ownerless history-proof publication harness | 🟡&nbsp;Partial | Focused production SQL coverage now asserts the controlled single-owner insert fast path has zero exact native history flush pages/fallback rounds, accepted rollback-segment proof samples matching published rollback-segment proof pages, accepted undo proof samples matching published undo proof pages, rollback-segment proof pages accounting for published native-support `FIL_PAGE_TYPE_SYS`, undo proof pages accounting for published native-support `FIL_PAGE_UNDO_LOG`, and native-support page-version records skipping live page-index publication; the unsafe ownerless hook build can force native-support page-version publication failure and proves that `trx_t::write_serialisation_history()` rejects the incomplete proof, accepts zero proof samples, takes positive native history flush pages, reports conservative COMMIT visibility because of publish failure, and still preserves same-handle, ownerless reopen, and forced-`.shm` native reopen visibility. This is a guardrail for future history-proof shrink/elision work, not a production speedup or a claim that the current proof pages are optional |
| Ownerless native-support proof-only WAL | 🟡&nbsp;Partial | Ownerless native-support history-proof records now append explicit proof-only page-log metadata carrying page identity, page LSN, commit LSN, and native-support state with zero page payload; primitive coverage proves proof-only append validation, latest/read rejection, replay skipping, checkpoint retained-callback skipping, and a session-scoped pair fast path that writes the same two proof-only native-support headers while preserving append/session record counts. The pair path now coalesces those adjacent zero-payload headers into one physical record-header write, while append statistics keep logical record counts and expose `record_header_write_calls` for probe attribution. Focused history/native-support SQL selectors preserve rollback-segment and undo proof publication counts and normal-build pair publication. The proof records remain durable and retained by existing WAL/checkpoint rules, but they are never indexed as page images; broader native redo/checkpoint reconciliation and DDL/file-lifecycle recovery remain planned |
| Ownerless index-delta first-base warm-up | 🟡&nbsp;Partial | Repeated file-per-table `FIL_PAGE_INDEX` page-log records can now use the existing non-chained delta encoding after one durable standalone base record instead of waiting for eight standalone observations; primitive coverage proves single-base delta selection, byte-exact direct/latest reads, retained-delta checkpoint rewrite to standalone records, post-checkpoint fallback, fast-delta counting, the `4096` byte fast-delta cap with exact fallback above the cap, and the bounded 32-delta base-refresh rule, while native-support history-proof publication and broader redo/checkpoint recovery remain planned |
| Ownerless undo-log page deltas | 🟡&nbsp;Partial | Repeated `FIL_PAGE_UNDO_LOG` page-log records now use an undo-specific non-chained delta flag against a durable standalone base record when the delta is smaller than the standalone payload; primitive coverage proves byte-exact direct/latest reads, retained undo-delta checkpoint rewrite to standalone records, post-checkpoint fallback, and fast-accepted undo delta counting, while the reduced production probe selected `0.752` undo-delta records per autocommit insert and cut undo-log page-log payload from `539.910` to `210.006` bytes/insert without changing the one rollback-segment plus one undo history-proof publication per insert; delta-base snapshots now avoid copying cached 16 KiB base pages before each index/undo delta encode by retaining immutable shared base references, a 500-row attribution sample after that change preserved the expected index/undo delta mix with `0.040` page-log encode ms/insert, the append perf probe now splits fast/exact delta acceptance plus fast-limit, standalone-comparison, and build-failure rejection reasons, exact fallback now reuses a fast-miss delta payload after the current standalone-size comparison accepts it instead of rebuilding the same payload, retained-delta exact fallback can use a size-only standalone probe to skip standalone payload materialization when the probe proves the delta wins, successful exact-probed delta appends refresh the process-local standalone-size estimate used by later fast decisions, the size-only probe now computes index/SYS compact-vs-fill sparse evidence in one pass while preserving exact delta rejection, repeated exact standalone rejections can skip the next exact standalone-size probe for the same delta-base slot while still storing standalone bytes, and the bounded fast-delta cap now admits proven `4096` byte-or-smaller deltas before exact fallback; replacing the remaining native-support proof requirement and broader redo/checkpoint recovery remain planned |
| Ownerless history-rseg page deltas | 🟡&nbsp;Partial | Explicitly marked rollback-segment history-proof pages with `FIL_PAGE_TYPE_SYS` or `FIL_PAGE_TYPE_TRX_SYS` can now use a history-rseg-specific non-chained delta flag against a durable standalone base record; ordinary unhinted SYS/TRX_SYS pages remain standalone, the rollback-segment and undo-header proof pages are still both published, primitive coverage proves hinted SYS and TRX_SYS delta reconstruction/latest lookup plus checkpoint rewrite to standalone records, the same bounded `4096` byte fast-delta cap applies before exact fallback, and performance-probe output reports history-rseg delta record and payload attribution, while replacing the remaining native-support proof requirement and broader redo/checkpoint recovery remain planned |
| Ownerless SYS page delta policy | 🟡&nbsp;Partial | `FIL_PAGE_TYPE_SYS` page-log records remain standalone for both core and user tablespaces after SYS-delta experiments proved unsafe: a broad 500-row probe selected `0.752` SYS deltas per ownerless autocommit insert and reduced page-log payload from `1112.386` to `1081.608` bytes/insert, but the record-lock-grant crash hook recovered `SUM(value)=30` instead of `31`, and a narrower user-tablespace attempt still failed from the repository-root working directory; primitive coverage now proves repeated `space_id=1` and `space_id=80` SYS records are not encoded as index or undo deltas and read back byte-identically, while broader native redo/checkpoint reconciliation remains planned before revisiting this optimization |
| Ownerless expanding-page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers distinct large-row updates across an expanding data-page set while a repeatable-read snapshot pin remains live, with retained page-version WAL during the pin, native checkpoint proof and WAL checkpoint after release, and ownerless/native reopen after forced `.shm` rebuild once the reader releases; no-live close-time reclamation also has deterministic coverage for a raw-latest/page-visible checkpoint gap during retained-WAL pressure; opt-in `ownerless_page_log_limit_bytes` write throttling covers direct/prepared writes including prepared `INSERT ... SELECT`, representative DML/DDL write classes, variant DML/upsert/index/rename/truncate spellings, DML modifier spellings, column ALTER variants, CHECK and FOREIGN KEY constraint DDL variants, storage/rebuild ALTER variants, AUTO_INCREMENT DDL high-watermark ALTER, generated-column DDL/index variants, generated-column FK DDL variants, and schema/table-copy/replacement/replacement-copy/view/trigger dictionary variants at the first user-visible WAL pressure limit, `mylite_ownerless_pressure_status()` exposes the current active-pin/WAL throttle state, thresholded ownerless write/DDL/transaction-end statement-boundary checkpoint scheduling is covered when no peer process is live; live-idle coverage proves native-support proof WAL remains retained while a peer is live and is reclaimed after the peer closes, while live writer and active-pin coverage retain user WAL, timer-driven checkpoint scheduling is covered after shared read-only snapshot release without another writer SQL statement and now proves an active prepared result cursor keeps retained WAL from being reclaimed until finalized, and deterministic active-reader pressure SQL trace export now provides external harness input with replacement-copy DDL and AUTO_INCREMENT high-watermark ALTER oracles plus bounded raw-client retry for MariaDB `1020`/`1205`/`1213` and SQLSTATE `40001` contention, plus Docker-backed MariaDB 11.8 scale-3 replay evidence, while full external MariaDB/RQG oracle stress remains planned |
| Ownerless pressure DDL variants | 🟡&nbsp;Partial | The active-reader pressure write-policy selector now also throttles column `ALTER TABLE ... MODIFY COLUMN`, `CHANGE COLUMN`, `DROP COLUMN`, `RENAME COLUMN`, `ALTER COLUMN ... SET DEFAULT`, and `ALTER COLUMN ... DROP DEFAULT`, CHECK constraint add/drop, FOREIGN KEY add/drop, charset conversion, `ALTER TABLE ... FORCE`, row-format rebuild, `ALTER TABLE ... AUTO_INCREMENT` high-watermark DDL, generated-column ALTER, generated-column secondary-index create/drop, stored generated-column child FK ADD, and stored generated-column referenced-FK DROP, plus `ALTER DATABASE`, duplicate `CREATE TABLE IF NOT EXISTS`, missing and real `DROP TABLE IF EXISTS`, `CREATE OR REPLACE TABLE ... LIKE`, `CREATE OR REPLACE TABLE ... AS SELECT`, `CREATE OR REPLACE VIEW`, `ALTER VIEW`, `CREATE OR REPLACE TRIGGER`, duplicate `CREATE TRIGGER IF NOT EXISTS`, and missing and real `DROP TRIGGER IF EXISTS` while retained WAL is at the configured ownerless pressure limit, verifies those blocked variants leave column metadata/defaults, CHECK and FK metadata, charset/collation, row-format metadata, AUTO_INCREMENT high-watermark state, generated-column, generated-index, and generated-column FK metadata, schema defaults, table state, replacement-copy target metadata, view projection, trigger bodies, and trigger presence unchanged, also verifies representative unsupported ownerless table-admin, `LOCK TABLES`, flush read-lock/export, host-file export/import, event/scheduler SQL including prepared event DDL/metadata, top-level sequence DDL/value SQL, `DISCARD TABLESPACE`, partitioned-table DDL, and rejected storage-option SQL keep explicit policy errors instead of pressure busy under the same retained-WAL limit, and verifies the same statement families succeed after reader release with final ownerless/native reopen before and after forced `.shm` rebuild |
| Ownerless BLOB page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers `ROW_FORMAT=DYNAMIC` off-page `LONGBLOB` payload updates that create native InnoDB BLOB page types, while a repeatable-read snapshot pin continues to read the original BLOB aggregates; coverage verifies retained page-version WAL during the pin, checkpoint after release, ownerless/native reopen before and after forced `.shm` rebuild for the final BLOB aggregates, and a bounded 12 KiB / 24 KiB / 48 KiB / 96 KiB / 192 KiB long-value size matrix, while exhaustive long-value limits, broader row-format, encryption, crash, and external oracle matrices remain planned |
| Ownerless compressed BLOB page pressure | 🟡&nbsp;Partial | Ownerless active-reader pressure now covers `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` off-page `LONGBLOB` payload updates created through prepared binary bindings that produce native InnoDB `ZBLOB`/`ZBLOB2` page types, while a repeatable-read snapshot pin continues to read the original compressed BLOB aggregates; coverage verifies retained page-version WAL during the pin, checkpoint after release, ownerless/native reopen before and after forced `.shm` rebuild for final aggregates, a bounded 12 KiB / 24 KiB / 48 KiB / 96 KiB / 192 KiB compressed long-value size matrix, and a bounded compressed `KEY_BLOCK_SIZE=1` / `2` / `4` / `8` / `16` matrix, while broader compressed DDL option combinations, encryption, crash, and external oracle matrices remain planned |
| Ownerless compressed row-format DDL refresh | 🟡&nbsp;Partial | Already-open ownerless peers now handle `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` table-copy rebuilds by observing the dictionary-generation change before page-version read selection, refreshing page-0 space-header flags with the rebuilt physical page size, evicting clean external pages, and staying in conservative native-read mode so stale pre-rebuild `(space_id, page_no)` page-version records are not overlaid onto the rebuilt compressed table; if the generation change is first noticed inside statement refresh, the same statement now revokes page-version-read eligibility before native execution; focused coverage verifies prepared BLOB writes after the peer rebuild, compressed row-format metadata, `ZBLOB`/`ZBLOB2` page evidence, post-peer parent refresh calls with zero `refresh_page_version_reads_enabled`, ownerless/native reopen, and forced `.shm` rebuild; `tools/ownerless-compressed-row-format-trace` now emits deterministic external-harness SQL for bounded `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`/`2`/`4`/`8`/`16` rebuild cycles with retry-aware repeatable-reader polling, per-round InnoDB `ZIP_PAGE_SIZE` metadata checks, and final metadata/aggregate oracles, with focused scale-2 Docker-backed MariaDB replay evidence for the widened key-block cycle plus existing full 12-family scale-2 replay evidence for the trace family, and hook-build crash coverage covers focused compressed row-format and key-block dictionary-boundary rebuilds, while rebuild-generation-aware page-version invalidation and external MariaDB/RQG stress remain planned |
| Ownerless BLOB pressure trace export | 🟡&nbsp;Partial | `tools/ownerless-blob-pressure-trace` emits deterministic external-harness SQL for `ROW_FORMAT=DYNAMIC` and `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` `LONGBLOB` pressure, including a retry-aware repeatable-read snapshot reader with bounded MariaDB `1020`/`1205`/`1213` and SQLSTATE `40001` handling, a retry-aware concurrent update worker with bounded MariaDB `1205`/`1213` and SQLSTATE `40001` handling, manifest metadata, and final aggregate oracles; it is part of the deterministic ownerless SQL trace suite and Docker-backed external MariaDB trace replay, with recorded MariaDB 11.8 scale-3 replay evidence for the combined dynamic/compressed BLOB pressure trace and full scale-2 replay evidence for the current 12 deterministic traces, while full external MariaDB/RQG long-running BLOB pressure stress remains planned |
| Ownerless CTAS DML trace export | 🟡&nbsp;Partial | `tools/ownerless-ctas-dml-trace` emits deterministic external-harness SQL for repeated `CREATE TABLE ... ENGINE=InnoDB AS SELECT` followed by post-create `UPDATE`, `DELETE`, and `INSERT` statements, a retry-aware repeatable-read snapshot reader over a stable aggregate table, manifest metadata, and final table/column/aggregate oracles; it is part of the deterministic ownerless SQL trace suite, has dependency-free generator, trace-runner, and suite check-mode coverage, and passed focused plus full-suite Docker-backed MariaDB 11.8 scale-2 replay for the current 12-trace suite (`trace_count=12`) after hardening the reader for raw-client `1020` contention, while randomized CTAS DML/RQG stress remains planned |
| Test-only directory-lock bypass | 🟡&nbsp;Partial | The `ownerless-test-hooks` preset still has an environment-controlled `mylite.lock` bypass for negative-proof tests; the bypass path now creates the MyLite-owned concurrency directory before taking the bootstrap byte-range lock for ordinary opens, a second process over the same directory must fail or hang within the bounded proof test, and the hook is unavailable in normal builds |
