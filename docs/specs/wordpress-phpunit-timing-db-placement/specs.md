# WordPress PHPUnit Timing DB Placement

## Problem

The WordPress PHPUnit job now uses production MyLite and MariaDB embedded
builds, and its build/setup/test phases are split into visible CI steps. Local
profiling still showed large timing swings when the prepared MyLite WordPress
database directory moved between the repository build tree and the harness
default external `/tmp` path. Those swings can make CI or branch/main
performance comparisons look like engine regressions when they are actually
database-directory placement differences.

This slice keeps the existing production-build guard and adds a timing
placement guard for CI: the WordPress job must keep its MyLite database
directory outside the checked-out repository. The harness also prints the
database parent filesystem type so timing logs expose whether the run used the
same storage class as the baseline.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` runs the WordPress job with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` and
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`, then separates `build-php`,
  dependency installation, `prepare-db`, `perf-probe`, `Tests_DB`,
  process-isolated tests, and remaining tests into individual steps.
- `tools/wordpress-phpunit-mysqli-mylite` defaults
  `MYLITE_WORDPRESS_DB_DIR` to
  `${TMPDIR:-/tmp}/mylite-wordpress-tests-<root-hash>.mylite`, which is outside
  the repository worktree and is mounted into the Docker container as
  `/mylite-wordpress-db/...`.
- Local production profiling on 2026-06-08 showed the same focused
  `Tests_Formatting_Emoji` process-isolated class was sensitive to DB
  placement:
  - with `MYLITE_WORDPRESS_DB_DIR=build/wordpress-tests.mylite`, the run
    passed in PHPUnit `58.153s`, shell real `88.594s`, and container total
    `94s`; child runtime was `48.516392s`, lock release `2.577176s`, and
    reconnect `2.641894s`;
  - with the default external `/tmp` database path, the run passed in PHPUnit
    `19.638s`, shell real `30.442s`, and container total `32s`; child runtime
    was `16.705641s`, lock release `1.075203s`, and reconnect `0.428487s`.
- A CI-sized WordPress mysqli performance probe showed the same direction:
  - repo-backed DB: PHP process plus connect/close `928.245 ms`,
    in-process connect/close `979.783 ms`, active-runtime reconnect
    `10.027 ms`;
  - default external DB: PHP process plus connect/close `606.114 ms`,
    in-process connect/close `437.155 ms`, active-runtime reconnect
    `5.716 ms`.

## Design

Add `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR`, defaulting to `0`, to
`tools/wordpress-phpunit-mysqli-mylite`.

When the option is set to `1`, the host-side harness rejects a
`MYLITE_WORDPRESS_DB_DIR` that resolves to the repository root or any path
inside it. The default `/tmp` path passes the guard. Local exploratory runs can
still use an in-repository database directory by leaving the option unset.

Pass the option through Docker and print:

- `wordpress_require_external_db_dir`,
- `wordpress_db_parent_filesystem_type`.

Enable `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1` in the WordPress CI job so
future timing steps fail early if a workflow edit puts the MyLite test database
under `build/`.

## Compatibility Impact

No SQL, C API, mysqli, PHP extension, native storage, or WordPress compatibility
behavior changes. The slice changes only the CI/harness timing environment.

## Directory And Lifecycle Impact

The WordPress PHPUnit harness continues to use a transient MyLite-owned test
database directory and to prepare it explicitly before test phases. CI now
requires that transient directory to be outside the repository checkout so
build artifacts and test database I/O do not share the same worktree path.

## Public API Impact

No public MyLite API changes.

## Native Storage Impact

No durable storage-format changes. Native MariaDB files are still created inside
the configured MyLite test database directory.

## Build And Performance Impact

No build graph changes. CI production timing remains based on Release MyLite
artifacts and MinSizeRel MariaDB embedded archives. The new guard improves
timing fidelity by preventing accidental use of an in-repository WordPress test
database directory for CI timings.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Verify the new guard rejects `MYLITE_WORDPRESS_DB_DIR=build/...` when
  `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1`.
- Run a default external-DB `prepare-db` phase with production build guards.
- Run a reduced production WordPress mysqli `perf-probe` and confirm
  `wordpress_db_parent_filesystem_type` and `wordpress_perf_summary_*` lines
  are printed.
- Run a focused process-isolated WordPress class and confirm child-process
  profile counters still print.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-08 used the production
`build/wordpress-php-embedded-prod` Release MyLite/PHP extension artifacts and
the `build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- A negative guard probe with
  `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1` and
  `MYLITE_WORDPRESS_DB_DIR=build/wordpress-tests.mylite` rejected the
  in-repository database path before Docker startup.
- Guarded default external-DB `prepare-db` passed with
  `wordpress_db_parent_filesystem_type=tmpfs` in the local container.
- Guarded CI-sized `perf-probe` passed and printed summary metrics including
  process plus connect/close `550.481 ms`, in-process connect/close
  `388.522 ms`, active-runtime reconnect `3.615 ms`, `SELECT 1`
  `268.45 ops/s`, prepared autocommit inserts `295.70 ops/s`, and direct
  autocommit inserts `691.32 ops/s`.
- Guarded focused `Tests_Formatting_Emoji` passed with PHPUnit `32.477s`,
  shell real `45.618s`, four child processes, child runtime `28.321397s`,
  lock release `1.699558s`, and reconnect `0.697941s`.
- `git diff --check` passed.
- `cmake --build --preset format-check-prod` passed.

## Acceptance Criteria

- WordPress CI requires the timing database directory outside the repository.
- The harness exposes the timing DB parent filesystem type in logs.
- In-repository DB paths are rejected only when the new guard is enabled.
- Production build guards remain unchanged.
- Docs record that DB placement, not just build type, affects WordPress timing
  comparisons.

## Risks And Unresolved Questions

- Requiring an external DB directory does not guarantee identical filesystem
  behavior across all hosted or self-hosted runners. The printed filesystem
  type makes that variance visible.
- This slice improves timing fidelity; it does not reduce MariaDB embedded
  startup cost or ownerless page-publication write volume.
