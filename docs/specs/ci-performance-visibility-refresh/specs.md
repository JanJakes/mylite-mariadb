# CI Performance Visibility Refresh

## Problem

The WordPress PHPUnit job now separates Docker image creation, WordPress fetch,
PHP extension builds, dependencies, database preparation, performance probing,
and the two PHPUnit suite partitions. The embedded CI job still grouped the two
embedded CTest invocations under one GitHub Actions step named `Test`, which
made the non-ownerless embedded tests and ownerless SQL shard group look like
one timing bucket.

The current performance question is also narrower than total CI wall time:
identify whether slow samples come from PHP process startup, ordinary mysqli
engine work, ownerless native coordination, or CI runner/setup variance.

## Source Findings

- `.github/workflows/ci.yml` already runs WordPress PHPUnit as separate
  test-only `^Tests_DB` and non-DB steps after build/setup phases.
- `.github/workflows/ci.yml` still ran both embedded CTest halves inside one
  `Test` step:
  `ctest --preset php-embedded-dev -LE compat.ownerless-cross-process-sql
  --parallel 2` followed by
  `ctest --preset php-embedded-dev -L compat.ownerless-cross-process-sql
  --parallel 2`.
- `packages/libmylite/CMakeLists.txt` registers ownerless SQL as sixteen
  weighted CTest shards with a 900-second shard timeout and a 300-second
  per-case watchdog inside the harness.
- `docs/specs/embedded-ctest-two-job-scheduling/specs.md` records the source
  evidence for keeping ownerless and non-ownerless embedded tests separated at
  two jobs rather than enabling full-preset parallelism.
- `tools/wordpress-phpunit-mysqli-mylite` exposes the `perf-probe` phase with
  process startup, process plus MyLite connect/close, in-process connect/close,
  read, point-select, transactional insert, prepared autocommit insert, and
  direct autocommit insert metrics.
- `mylite_embedded_performance_probe` exposes the lower-level C API path for
  ordinary and ownerless open/close, direct/prepared reads, transactional
  writes, autocommit writes, and opt-in ownerless native timing counters.

## Design

Split the GitHub Actions step boundary and keep the stable scheduling for each
half:

- `Run embedded non-ownerless tests` runs the `-LE
  compat.ownerless-cross-process-sql` half with `--parallel 2`.
- `Run embedded ownerless SQL tests` asks the harness for `sql-case-count` and
  runs each case through `sql-case <index>` with `/tmp` ownerless cleanup
  between cases.

Both steps add `--output-on-failure` so failures show the relevant CTest output
in the step that timed out or failed. This changes CI observability and
scheduling only; no MyLite runtime behavior or SQL behavior is added.
Ownerless SQL still runs the same 161 direct cases, with a few live-peer
body-materialization/readback assertions narrowed after they reproduced
sequence-pressure hangs while the same metadata and durable-state oracles
remained covered.

The ownerless SQL step uses isolated case invocations because current-head
verification found paired ownerless shard execution, one-shot full-label
serial execution, and isolated shard execution load-sensitive. One full
`--parallel 2` ownerless run reached 15 of 16 passing shards before shard `.0`
timed out in
`test_ownerless_index_idempotent_ddl_refreshes_peer_dictionary`; that case
passed directly in `5 sec`, and shard `.0` passed alone in `41.58 sec`. A
follow-up paired run then timed out inside shard `.14` at
`test_ownerless_view_prepared_dml_enforces_check_option`. A one-shot serial
full-label run later timed out inside shard `.9` at
`test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`, while
that shard passed alone in `41.21 sec`. An isolated shard `.3` run then timed
out at `test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary`,
while that case passed directly in `4 sec`. Running each ownerless case through
the harness's direct `sql-case` path keeps the CI timing bucket visible,
preserves per-case seconds in logs, and avoids the failing aggregate execution
shape until the ownerless DDL/view, prefix-index, and foreign-key hot paths have
a runtime fix.

## Performance Findings

Current-head local profiling on 2026-06-08 used branch
`ownerless-concurrency` at `2856eae3`, warmed local build artifacts, and the
existing WordPress Docker image.

The WordPress mysqli `perf-probe` reported:

- stock PHP process startup: `170.168ms`;
- PHP wrapper startup with MyLite extensions loaded: `110.371ms`;
- PHP process plus MyLite connect/close: `557.058ms`;
- derived process-plus-connect delta: `446.687ms`;
- in-process mysqli connect/close: `388.856ms`;
- `SELECT 1`: `254.47 ops/s`;
- transactional prepared inserts: `430.61 ops/s`;
- primary-key point selects: `238.04 ops/s`;
- prepared autocommit inserts: `355.05 ops/s`;
- direct-string autocommit inserts: `665.37 ops/s`.

The lower-level embedded C API probe at full durability with ownerless stats
enabled reported:

- ordinary warm open/close: `666.928ms`;
- ownerless warm open/close: `580.139ms`;
- ordinary direct `SELECT 1`: `3943.54 ops/s`;
- ordinary prepared `SELECT 1`: `2172.89 ops/s`;
- ordinary transactional inserts: `1850.24 ops/s`;
- ordinary autocommit inserts: `1741.41 ops/s`;
- ownerless direct `SELECT 1`: `3059.12 ops/s`;
- ownerless prepared `SELECT 1`: `2057.92 ops/s`;
- ownerless transactional inserts: `1231.38 ops/s`;
- ownerless autocommit inserts: `77.76 ops/s`.

A durability-off embedded probe improved ownerless autocommit to
`114.74 ops/s` while ordinary autocommit stayed around `1984.74 ops/s`.
That shows durable sync cost is only part of the ownerless autocommit gap.
The remaining measured ownerless work is concentrated in page-write refresh,
page-version reads, page-log append, page-read misses, and visibility
publication. Two low-risk experiments were rejected before this slice was
finalized: increasing the page-write refresh direct-mapped cache from 64 to
256 entries did not materially reduce misses, and reusing a thread-local
aligned refresh buffer did not improve the measured wall time.

## Compatibility Impact

No SQL, PHP API, public C API, storage format, or native storage behavior
changes. The workflow split keeps the same non-ownerless CTest label filter,
the same ownerless SQL case coverage through the harness, and the same
WordPress PHPUnit partitioning.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the WordPress `perf-probe` phase with non-default iteration counts.
- Run the stats-enabled embedded C API performance probe at full durability.
- Run a durability-off embedded C API performance probe to separate fsync cost
  from ownerless coordination cost.
- Run `ctest --preset php-embedded-dev -N` to validate the workflow CTest label
  filters still discover tests.
- Run isolated ownerless SQL timeout cases through
  `mylite_ownerless_cross_process_sql_test sql-case <name>`, then run the full
  direct-case loop using `sql-case-count` to separate deterministic case
  failures from runner-load scheduling failures.
- Run focused embedded CTest halves matching the workflow commands.
- Run `cmake --build --preset format-check`.
- Run `git diff --check`.

## Acceptance Criteria

- GitHub Actions shows separate embedded non-ownerless and ownerless SQL step
  timings.
- WordPress PHPUnit remains split from build/setup and from the non-DB suite.
- Ownerless SQL keeps a visible CI step timing and emits per-case harness
  timings while running each case in an isolated `sql-case` invocation until
  aggregate ownerless shard execution is fixed at the runtime level.
- The current performance evidence separates PHP process startup, ordinary
  mysqli engine work, ordinary C API work, and ownerless C API work.
- No unmeasured ownerless hot-path code experiment is committed without a
  clear improvement and focused correctness coverage.

## Verification Notes

- The direct ownerless SQL loop reported `sql-case-count=161` and passed cases
  `0` through `160` with per-case start/pass timing lines.
- The non-ownerless embedded CI half passed `48/48` with
  `ctest --preset php-embedded-dev -LE compat.ownerless-cross-process-sql
  --parallel 2 --output-on-failure` in `68.23 sec`.
