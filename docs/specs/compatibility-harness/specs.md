# Compatibility Harness

## Problem

MyLite now has meaningful coverage spread across first-party unit tests,
embedded lifecycle tests, and the opt-in storage-engine smoke profile. The
coverage is useful, but it is not grouped by compatibility claim. That makes it
too easy to say "tests pass" without knowing whether the public API, storage
format, recovery, locking, sidecar, or routed SQL claim was actually exercised.

This slice adds a first compatibility harness that runs existing coverage by
compatibility surface. It does not replace CTest or MariaDB's own test runner;
it gives MyLite a stable layer above them for repeatable project-level groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/mariadb-test-run.pl` is MariaDB's MTR entry point for
  `.test` suites and server-oriented compatibility coverage.
- `mariadb/mysql-test/CMakeLists.txt` wires MTR custom targets, including
  embedded-server runs when MariaDB is built with `WITH_EMBEDDED_SERVER`.
- `mariadb/cmake/ctest.cmake` exposes a lightweight pattern for CTest-backed
  upstream unit tests.
- MyLite's first-party tests already use CTest presets for the `dev`,
  `embedded-dev`, and `storage-smoke-dev` profiles.

MTR is the right long-term source of MariaDB comparison cases, but it assumes
MariaDB's server test layout and lifecycle. The first MyLite harness should
therefore keep using CTest around MyLite-owned tests and leave MTR integration
for a later comparison slice.

## Design

Add CTest labels for compatibility surfaces that already have committed tests:

- `compat-public-api`,
- `compat-storage`,
- `compat-crash-recovery`,
- `compat-locking`,
- `compat-embedded-lifecycle`,
- `compat-direct-sql`,
- `compat-storage-engine`,
- `compat-sidecar`,
- `compat-routed-ddl-dml`.

Add `tools/mylite-compat-harness` as the project-level runner. It maps human
group names to `(preset, CTest label)` pairs, prepares the MariaDB embedded
archive when a group requires it, builds the matching preset, and runs CTest
with the selected label. A `report` command writes a small Markdown status
table under `build/`.

## Supported Scope

- Public C API validation.
- Storage format, catalog, row, index, mutation, recovery, and locking unit
  coverage.
- Embedded open/close, direct execution, and runtime cleanup coverage.
- Static MyLite handler registration and routed DDL/DML smoke coverage.
- Forbidden durable sidecar gates through the storage-engine smoke test.

## Non-Goals

- Full MTR integration.
- Automatic comparison against an external MariaDB server process.
- WordPress or ORM application-schema suites.
- Coverage scoring, requirement traceability, or dashboards beyond a local
  Markdown report.
- CI service integration.

## Compatibility Impact

The compatibility matrix does not gain new supported SQL behavior from this
slice. The change is validation infrastructure: compatibility claims can now be
run by surface instead of by raw test binary or preset.

## File-Lifecycle Impact

No database file-format change is introduced. The harness prepares build
artifacts under `build/` and reuses existing MyLite test-owned temporary
directories and `.mylite` files.

## Build, Size, And Dependencies

No new dependency is introduced. The runner is a POSIX shell script, consistent
with `tools/mariadb-embedded-build`. The MariaDB archive size does not change.

## Test Plan

- Verify the harness lists all supported groups.
- Run a cheap dev-backed group.
- Run embedded-backed groups to verify archive preparation and CTest labels.
- Run storage-smoke-backed groups to verify the static handler archive path.
- Run the existing dev, embedded, storage-smoke, tidy, format, diff, and archive
  checks before committing.

## Acceptance Criteria

- Existing tests have compatibility labels.
- `tools/mylite-compat-harness list` shows the implemented groups.
- `tools/mylite-compat-harness run <group>` configures/builds/runs the expected
  preset and label.
- `tools/mylite-compat-harness report <group...>` writes a Markdown report and
  returns non-zero on any failed group.
- Roadmap and architecture docs describe the initial harness and remaining MTR,
  MariaDB comparison, and application-schema work.

## Risks

- Several groups initially point at the same broad test binaries. That is
  acceptable for the first harness, but future slices should split tests or add
  narrower fixtures as compatibility coverage grows.
- Automatically preparing MariaDB archives makes storage-smoke groups heavier
  than ordinary unit groups. The cost is explicit in the group preset and can be
  optimized later with CI caching.
