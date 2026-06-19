# CI Performance Artifacts

## Problem

The ownerless concurrency branch now separates production build, setup,
performance-probe, and PHPUnit test-only CI steps. That makes timings visible
in the Actions UI, but the most useful performance evidence still lives mainly
in step logs or the GitHub step summary. When a job is still running, GitHub
does not expose partial logs through `gh run view --log`, and after completion
the embedded probe summaries require log scraping.

The next performance loop needs stable artifacts for branch/main comparison:
embedded production probe logs and the WordPress timing summary should be
downloadable without enabling heavyweight diagnostic profilers or changing the
timing path.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `.github/workflows/ci.yml` runs the embedded performance probe, large-row
  bulk probe, and ownerless attribution probe before embedded ownerless SQL
  tests under `php-embedded-prod`, guarded by the `MinSizeRel` MariaDB archive
  check and first-party `Release` build check.
- `.github/workflows/ci.yml` already writes
  `build/wordpress-phpunit-reports/timing-summary.md` and prints it in the
  `Show WordPress timing summary` step.
- `tools/check-ci-production-builds` owns the workflow guardrails for
  production build types, split WordPress phases, disabled heavyweight
  profiling, and embedded probe order.

## Design

Keep the CI timing paths production-shaped and unprofiled by default.

For the embedded job:

- create `build/embedded-performance-reports`;
- pipe each production embedded probe through `tee` into a stable log:
  - `default.log`,
  - `large-row-bulk.log`,
  - `ownerless-attribution.log`;
- upload that directory through `actions/upload-artifact@v4` with
  `if: always()` so completed probe logs remain downloadable even if a later
  embedded correctness step fails.

For the WordPress job:

- keep publishing the timing summary to the step summary;
- upload `build/wordpress-phpunit-reports/timing-summary.md` through
  `actions/upload-artifact@v4` with `if: always()`.

Extend the production-build audit so CI fails if the artifact steps disappear,
if embedded probe output stops being written to the expected files, or if the
artifact paths/names change without updating the guard.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli API, wire-protocol, native storage,
directory-layout, or ownerless concurrency behavior changes. This is CI
diagnostic evidence handling only.

## Build And Performance Impact

The probe commands already print their full output to stdout. Piping through
`tee` adds file writes outside the measured probe loops and does not enable
extra instrumentation. The WordPress timing artifact reuses the already written
summary file. Normal CI keeps JUnit logging, child profiling, and mysqli
profiling disabled on timing paths.

## Test Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run prove the Actions artifact steps execute in
  GitHub's environment.

## Acceptance Criteria

- Embedded production probe output remains visible in logs and is also
  persisted as `embedded-performance-reports`.
- The WordPress timing summary remains visible in the step summary and is also
  persisted as `wordpress-phpunit-timing-summary`.
- The production-build audit requires the artifact steps and expected paths.
- No heavyweight diagnostic profiler is enabled on default timing paths.

## Verification Results

Local verification completed:

```text
bash -n tools/check-ci-production-builds
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
cmake --build --preset format-check-prod
git diff --check
```

The production-build audit passed and reported the guarded workflow path. The
focused production CTest wrapper passed one `tools.ci-production-builds` test.
The next pushed CI run remains the GitHub-environment proof that the
`actions/upload-artifact@v4` steps create the expected downloadable artifacts.

## Risks And Follow-Up

GitHub artifact upload is only exercised in Actions, so local verification can
only audit the workflow text. If artifact upload overhead becomes visible, the
step can be moved later in each job, but it should stay outside measured probe
commands and keep `if: always()` so failed correctness steps preserve earlier
performance evidence.
