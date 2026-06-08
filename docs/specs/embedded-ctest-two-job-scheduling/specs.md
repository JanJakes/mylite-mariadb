# Embedded CTest Two-Job Scheduling

## Problem Statement

The ownerless SQL weighted-shard slice proved the ownerless SQL label itself
can run at two CTest jobs locally, reducing wall time from about 609 seconds to
about 312 seconds. CI still invokes `ctest --preset php-embedded-dev` without
parallel jobs, so that evidence does not automatically speed up the embedded
CI path.

MyLite needs a separate evidence gate for full embedded preset scheduling. If
the full preset passes at two jobs, enable only the proven preset path. If it
does not, keep global full-preset parallelism disabled and only enable a split
path when each half has local evidence.

## Source Findings

- `.github/workflows/ci.yml` runs the embedded CI job with
  `ctest --preset php-embedded-dev`.
- `CMakePresets.json` currently gives `embedded-dev` and `php-embedded-dev`
  test presets no `execution.jobs` value, so CTest runs serially unless the
  caller passes `-j`/`--parallel`.
- `packages/libmylite/CMakeLists.txt` now registers ownerless SQL tests as
  weighted shards through `sql-weighted-shard`.
- `docs/specs/ownerless-sql-weighted-shards/specs.md` records passing serial
  and two-job ownerless SQL label measurements.

## Design

Do not assume global CTest parallelism is safe. Measure the exact full preset
path before changing it:

1. Run the full embedded preset with two jobs locally.
2. If ownerless and non-ownerless tests interfere, split CI into two explicit
   CTest invocations: non-ownerless tests at two jobs, then ownerless SQL
   weighted shards at two jobs.
3. Keep CMake presets serial by default until full-preset parallelism is safe
   outside CI's split execution shape.
4. Keep ownerless stress and unsafe-hook presets unchanged unless separately
   measured.

## Scope

In scope:

- CTest preset scheduling and CI documentation for embedded tests.
- Evidence recording for full preset runtime and failures.
- The embedded CI `ctest --preset php-embedded-dev` step.

Out of scope:

- Ownerless SQL weighted-shard implementation, already covered separately.
- Product SQL/storage/runtime behavior.
- Higher parallelism than two jobs.
- WordPress PHPUnit runtime behavior, which is handled by the separate
  WordPress harness evidence.

## Compatibility Impact

No MySQL/MariaDB compatibility behavior changes. This slice changes CI test
execution scheduling only for the locally measured split path.

## Test Plan

- Run `ctest --preset embedded-dev -j2 --output-on-failure`.
- If available, run `ctest --preset php-embedded-dev -j2 --output-on-failure`
  or a focused equivalent that includes ownerless SQL plus PHP extension tests.
- Update CI only for the proven split path.
- Rerun CTest discovery and a focused ownerless weighted shard after any preset
  change.
- Run `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- No CI preset gains two-job scheduling without a passing local full-preset or
  focused equivalent measurement.
- Ownerless weighted shards remain registered and runnable.
- Docs record the exact command, wall time, and any failure if full two-job
  scheduling is still unsafe.

## Evidence

Full embedded two-job scheduling is still unsafe. This command failed after an
ownerless shard interleaved with many non-ownerless embedded tests:

```text
ctest --preset embedded-dev -j2 --output-on-failure
```

Failure:

```text
libmylite.ownerless-cross-process-sql.1
ownerless-sql case timeout index=74
name=test_ownerless_row_format_ddl_refreshes_peer_dictionary
timeout_seconds=300
Total Test time (real) = 495.38 sec
```

The split non-ownerless embedded half passed:

```text
ctest --preset embedded-dev -LE compat.ownerless-cross-process-sql -j2 --output-on-failure
45/45 tests passed
Total Test time (real) = 59.75 sec
```

The ownerless SQL half already passed in the weighted-shard slice:

```text
ctest --preset embedded-dev -L compat.ownerless-cross-process-sql -j2 --output-on-failure
8/8 tests passed
Total Test time (real) = 312.45 sec
```

CI therefore uses the same split with the `php-embedded-dev` preset. A later
CI visibility refresh moved the commands into separate GitHub Actions steps so
the non-ownerless embedded half and ownerless SQL half have independent visible
timings. The non-ownerless half remains two-job, while the ownerless SQL half
runs each harness case through the hidden-child `sql-case` path with `/tmp`
ownerless cleanup between cases because paired and one-shot serial ownerless
shard execution both exposed load-sensitive per-case watchdogs even when the
timed-out cases passed directly:

```sh
ctest --preset php-embedded-dev -LE compat.ownerless-cross-process-sql --parallel 2 --output-on-failure
ownerless_sql_test=build/php-embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
ownerless_sql_case_count="$("$ownerless_sql_test" sql-case-count)"
for case_index in $(seq 0 "$((ownerless_sql_case_count - 1))"); do
  rm -rf /tmp/mylite-ownerless-sql.*
  "$ownerless_sql_test" sql-case "$case_index"
done
```

A full two-job ownerless SQL run first reached 15 of 16 passing shards and
then timed out inside shard `.0` at
`test_ownerless_index_idempotent_ddl_refreshes_peer_dictionary`; that case
passed directly in `5 sec`, and shard `.0` passed alone in `41.58 sec`. A
follow-up run with shard `.0` isolated then timed out inside shard `.14` at
`test_ownerless_view_prepared_dml_enforces_check_option`. A one-shot serial
full-label ownerless run later timed out inside shard `.9` at
`test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`, while
that same shard passed as an isolated CTest invocation in `41.21 sec`. The
same sequence pressure later appeared in an isolated shard `.3` invocation at
`test_ownerless_foreign_key_child_rename_refreshes_peer_dictionary`, while
that case passed directly in `4 sec`. The current CI split therefore keeps the
timing buckets visible and runs each ownerless case in the passing direct
hidden-child shape until the ownerless DDL/view, prefix-index, and foreign-key
hot paths have a runtime fix.

CTest discovery for the `php-embedded-dev` preset confirmed the same label
boundary at the time of this slice: ownerless SQL shards under
`compat.ownerless-cross-process-sql` and the non-ownerless tests outside that
label. Later shard-count tuning keeps the same label boundary while changing
the number of registered ownerless SQL tests.

## Risks And Open Questions

- Full `php-embedded-dev` may add PHP extension tests whose process and
  environment assumptions differ from the ownerless SQL label.
- Two-job local evidence is necessary but not sufficient for every hosted CI
  machine; CI results still need monitoring after the change.
