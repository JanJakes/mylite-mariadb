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

CI therefore uses the same split with the `php-embedded-dev` preset:

```sh
ctest --preset php-embedded-dev -LE compat.ownerless-cross-process-sql --parallel 2
ctest --preset php-embedded-dev -L compat.ownerless-cross-process-sql --parallel 2
```

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
