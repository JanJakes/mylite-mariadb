# CI Ownerless SQL Case Timings

## Problem

The production embedded CI job already runs performance probes before
correctness tests, but the `Run embedded ownerless SQL tests` step executes the
whole ownerless SQL direct-case loop inside one GitHub Actions step. When this
step is slow, times out, or fails after many cases, CI exposes only the final
step duration unless a reviewer scrapes the raw log. That makes it harder to
separate startup/process cost, a single slow ownerless case, and broad engine
regression.

## Source Findings

- MyLite targets MariaDB 11.8 LTS from base tag `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not change
  MariaDB source or storage-engine semantics.
- `.github/workflows/ci.yml` runs `mylite_embedded_performance_probe` and the
  ownerless attribution probe before correctness coverage, then runs
  `mylite_ownerless_cross_process_sql_test sql-case <index>` for every case
  reported by `sql-case-count`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` implements the
  `sql-case-count` and `sql-case <index-or-name>` commands. Direct case
  execution is therefore the right unit for timing the existing CI loop without
  changing the test binary or case scheduling.
- `tools/check-ci-production-builds` already audits that timing-producing CI
  steps keep `Release` first-party MyLite builds and the `MinSizeRel` embedded
  MariaDB archive. The same audit should guard any new CI timing surface.

## Design

The embedded ownerless SQL CI step records the case count and wraps every
direct `sql-case` invocation with wall-clock timing. Each iteration prints a
start marker before `/tmp/mylite-ownerless-sql.*` cleanup and a seconds/status
marker after the test binary exits. The wrapper captures a nonzero case status,
writes the timing row, and then exits with that status so failures are still
reported by GitHub Actions exactly as before.

When `GITHUB_STEP_SUMMARY` is available, the step also appends a compact
Markdown table with case index, exit status, and elapsed seconds. The table is
case-index based rather than name based because `sql-case-count` is currently
the workflow's stable loop source, while direct rerun still supports either
index or name through the existing test binary.

## Affected Subsystems

- CI workflow only.
- Production-build/timing audit tool.
- Documentation of ownerless performance evidence.

## Compatibility Impact

No MySQL or MariaDB SQL, C API, storage, protocol, or wire behavior changes.
The existing ownerless SQL test cases run in the same order and with the same
temporary-directory cleanup. The only externally visible change is additional
CI log and GitHub step-summary output.

## Database Directory And Embedded Lifecycle Impact

None. The workflow keeps the existing `/tmp/mylite-ownerless-sql.*` cleanup
between direct cases and does not alter MyLite database directory ownership,
embedded runtime startup, shutdown, or native storage behavior.

## Native Storage Impact

None. The slice records timing around existing tests and does not alter InnoDB,
Aria, MyISAM, redo, checkpoint, or ownerless page-version paths.

## Build, Size, License, And Dependency Impact

No binary, dependency, license, or build-profile impact. The workflow uses POSIX
shell builtins plus `date +%s`, already available in the GitHub Actions Ubuntu
environment used by the project.

## Test And Verification Plan

- `bash -n tools/check-ci-production-builds`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The slice intentionally does not rerun the full ownerless SQL suite because it
does not change the test binary or ownerless runtime behavior. CI will produce
the first full per-case timing table for the pushed commit.

## Acceptance Criteria

- The embedded ownerless SQL CI step prints `ownerless_sql_case_start` and
  `ownerless_sql_case_seconds` markers for every direct case.
- Failed cases still cause the step to fail, after writing the failed case's
  duration and status.
- GitHub step summary includes an embedded ownerless SQL timing table when
  Actions provides `GITHUB_STEP_SUMMARY`.
- `tools/check-ci-production-builds` fails if the new timing markers or summary
  surface are removed from the workflow.

## Risks And Unresolved Questions

- `date +%s` gives whole-second resolution. That is enough for the current
  long-step diagnosis; subsecond timing can be added later if case-level
  variance is small enough to need it.
- Case indexes are less descriptive than case names in the summary. The current
  direct loop uses indexes, and failures remain rerunnable by index. A later
  test-binary enhancement can expose case names without weakening this slice.
- This does not optimize PHPUnit, embedded startup, or ownerless engine paths
  by itself. It makes the next optimization target measurable in CI.
