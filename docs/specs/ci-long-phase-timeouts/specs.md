# CI Long Phase Timeouts

## Problem

The ownerless branch depends on production CI timing evidence, but a pushed run
can still remain live for hours inside a long setup phase before producing logs
or PHPUnit timing summaries. The latest observed stuck shape was not a failed
ownerless SQL assertion or PHPUnit regression: GitHub still reported
`ubuntu-embedded` in `Build MariaDB embedded archive` and
`wordpress-phpunit-mysqli-mylite` in `Build MyLite PHP extensions for WordPress
PHPUnit`, while smaller matrix and clang jobs had already completed.

That failure mode blocks performance work because the run neither gives a
completed timing sample nor fails quickly enough to show which phase regressed.

## Source Findings

- `.github/workflows/ci.yml` already uses production presets for timing-bearing
  jobs: normal matrix jobs use `prod`, embedded jobs use `php-embedded-prod`,
  the MariaDB embedded archive uses the documented `MinSizeRel` baseline, and
  WordPress PHP builds use `Release`.
- The WordPress job already has a 90-minute job timeout and is split into
  Docker image, source fetch, PHP build, dependency install, database
  preparation, performance probe, and test-only PHPUnit phases.
- The embedded job did not have a job-level timeout. Its long archive build,
  embedded PHP build, non-ownerless CTest, and direct ownerless SQL case-loop
  phases also had no step-level timeout.
- The WordPress `build-php` phase is a separate visible step, but it did not
  have its own phase timeout. A cold run can legitimately build MariaDB and PHP
  extension targets, but it should not silently consume the whole job budget.
- `tools/check-ci-production-builds` already audits production-build markers in
  workflow step bodies, so it is the local guard that should also prevent timing
  timeout markers from being removed accidentally.

No MariaDB source behavior changes in this slice. The relevant source authority
is the CI workflow and existing MyLite build harnesses:

- `.github/workflows/ci.yml`
- `tools/mariadb-embedded-build`
- `tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`

## Design

Add explicit CI bounds around the long phases whose timings matter:

- `ubuntu-embedded` job timeout: 75 minutes.
- `Build MariaDB embedded archive`: 30 minutes.
- `Build embedded PHP targets`: 15 minutes.
- `Run embedded non-ownerless tests`: 10 minutes.
- `Run embedded ownerless SQL tests`: 45 minutes.
- `Build MyLite PHP extensions for WordPress PHPUnit`: 30 minutes.

Keep the existing WordPress 90-minute job timeout. The step timeouts are
deliberately larger than documented successful timings so normal variance does
not fail good runs, but small enough that a stuck build/test phase fails with a
useful CI boundary instead of hiding progress for hours.

Rename the embedded job's generic `Build` step to `Build embedded PHP targets`
so timeout auditing can match the intended step unambiguously.

Extend `tools/check-ci-production-builds` to require the long-phase timeout
markers. The script remains the single CI workflow audit invoked by every job.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, native-format, or
database-directory behavior changes. This slice changes GitHub Actions
orchestration and local workflow auditing only.

## Directory And Lifecycle Impact

No MyLite-owned database directory behavior changes. Temporary CI build,
WordPress, Composer, and test database paths remain unchanged.

## Public API Impact

None.

## Native Storage Impact

None.

## Wire-Protocol Or Integration-Package Impact

No runtime integration behavior changes. The WordPress mysqli harness keeps the
same production build, performance probe, and split PHPUnit phases.

## Build And Performance Impact

The production build configuration is unchanged. The timeouts do not optimize
MariaDB or MyLite execution; they make slow or stuck build/test phases produce a
bounded failure and preserve actionable timing visibility for performance work.

The chosen limits are intentionally phase-specific:

- archive and WordPress `build-php` timeouts cover cold embedded builds,
- embedded PHP target build timeout covers first-party compilation after the
  archive is already available,
- non-ownerless CTest timeout covers the documented short embedded test half,
- ownerless SQL timeout covers the direct case-loop that is expected to be much
  longer than the non-ownerless half.

## Test And Verification Plan

- Run `tools/check-ci-production-builds`.
- Run `bash -n tools/check-ci-production-builds`.
- Run `git diff --check`.
- Inspect the workflow diff to confirm the production presets and guard steps
  remain unchanged.

Full embedded and WordPress CI runs remain the external verification for the
actual timeout behavior because GitHub Actions enforces `timeout-minutes`.

## Acceptance Criteria

- The embedded job has an explicit job-level timeout.
- The long embedded archive/build/test phases have step-level timeouts.
- The WordPress PHP build phase has a step-level timeout while keeping the
  existing job timeout.
- `tools/check-ci-production-builds` fails if the long-phase timeout markers
  are removed.
- No runtime, SQL, API, or native storage behavior changes are made.

## Risks And Unresolved Questions

- A heavily loaded GitHub runner could still exceed a timeout during a valid
  cold build. The limits are conservative relative to prior documented CI
  timings, and a timeout failure is more useful than an unbounded pending run.
- GitHub does not publish logs for still-running job steps through `gh api`
  until logs are materialized. These bounds make that missing-log state finite
  rather than trying to scrape live step output.
