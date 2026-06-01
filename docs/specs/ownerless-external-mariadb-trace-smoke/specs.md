# Ownerless External MariaDB Trace Smoke

## Problem Statement

The deterministic ownerless SQL trace suite can now validate every generated
trace package and optionally replay it through a caller-supplied SQL client.
The remaining external-oracle gap still needs real MariaDB/RQG-style execution.
Default CI should not depend on a daemon, Docker, image pulls, or long-running
external infrastructure, but MyLite needs a reproducible opt-in path that runs
the deterministic suite against a disposable external MariaDB server.

## Source Findings

- `tools/ownerless-sql-trace-suite` can replay the generated trace suite through
  any command-line client by passing `--client` and repeated `--client-arg`
  options.
- The official MariaDB container image includes both `mariadbd` and the
  `mariadb` command-line client, so Docker can supply a disposable external
  server and a client without requiring host-level MariaDB packages.
- Generated traces create and mutate an `app` database and should run only
  against a disposable external environment.

## Design

Add `tools/ownerless-external-mariadb-trace-smoke`:

1. Accept an output directory, MariaDB image name, optional container name,
   root password, startup timeout, `--keep-container`, and `--check`.
2. In `--check` mode, validate the command plan without using Docker.
3. In run mode, require Docker and an available daemon, create a unique
   disposable MariaDB container, wait for `mariadb-admin ping`, and run
   `tools/ownerless-sql-trace-suite` against the container through
   `docker exec -i <container> mariadb -uroot -p...`.
4. Use the smoke-safe suite subset by default, skipping retry-sensitive
   `fk-graph` and `active-reader-pressure` traces because raw SQL clients cannot
   retry MariaDB deadlocks the way the in-process ownerless stress harness can.
   `--full-suite` keeps an escape hatch for environments that add their own
   retry/oracle handling.
5. Store generated traces under `output/traces`, runner logs under
   `output/logs`, and external execution metadata in
   `output/external-manifest.txt`.
6. Remove the container on exit unless `--keep-container` is supplied.
7. Register only the dependency-free `--check` mode as a normal CMake smoke
   test.

## Scope

In scope:

- Opt-in deterministic trace-suite smoke replay against a disposable external
  MariaDB Docker container.
- Check-mode CMake coverage that does not require Docker.
- Documentation and compatibility matrix updates.

Out of scope:

- Default CI Docker/image-pull dependency.
- RQG random generation.
- Long-running external stress.
- Managing non-Docker MariaDB installations.
- Changing product ownerless concurrency behavior.

## Compatibility Impact

No product SQL behavior changes. The slice adds a reproducible bridge for
environment-owned external MariaDB validation of deterministic ownerless trace
packages.

## Directory And Lifecycle Impact

No MyLite database directory layout changes. The tool creates only the requested
output directory and a disposable Docker container. It mutates the external
container's `app` database.

## Native Storage Impact

No MyLite native storage changes.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds a shell tool and CMake
check-mode smoke test.

## Test Plan

- Run `bash -n tools/ownerless-external-mariadb-trace-smoke`.
- Run
  `tools/ownerless-external-mariadb-trace-smoke --output DIR --check`.
- Run the focused CMake smoke test with
  `ctest --preset embedded-dev -R 'tools\\.ownerless-external-mariadb-trace-smoke-check'`.
- If Docker is available, run the tool without `--check` against the default
  MariaDB image.
- Run the broader ownerless trace tool filter, `format-check`,
  `git diff --check`, cached diff checks, and cleanup checks.

## Acceptance Criteria

- Check mode succeeds without Docker.
- Run mode starts a disposable MariaDB container, waits for readiness, replays
  the smoke-safe deterministic ownerless SQL trace subset, writes logs and a
  manifest, and removes the container by default.
- The compatibility docs continue to mark full MariaDB/RQG long-running stress
  as planned.

## Risks And Open Questions

- Docker availability, image pull behavior, and host resource limits are
  environment-owned.
- The smoke run is deterministic and bounded; it is not a substitute for
  randomized RQG or long-running application-oracle stress.
- The raw-client smoke deliberately skips FK graph and active-reader pressure by
  default because those traces can expose ordinary MariaDB deadlocks without a
  retry runner.
