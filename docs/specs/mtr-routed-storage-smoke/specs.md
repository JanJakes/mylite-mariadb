# MTR Routed Storage Smoke

## Goal

Add an opt-in MariaDB MTR smoke mode that runs a MyLite-owned test against the
static MyLite storage engine with an active primary `.mylite` file. The first
test should prove that raw embedded MTR execution can route `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, `ENGINE=MEMORY`, `ENGINE=HEAP`, and omitted
engine DDL through MyLite storage when the session forces MyLite as the
effective storage engine.

## Non-Goals

- Moving the existing baseline MTR list to MyLite storage.
- Running upstream `main` MTR tests against MyLite storage.
- Replacing `libmylite` public API tests or compatibility-harness routed SQL
  tests.
- Claiming broad MTR-scale comparison coverage.
- Writing durable state to MariaDB engine sidecars as a MyLite storage
  compatibility shortcut.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc` registers the static MyLite handler and
  exposes a read-only `mylite_primary_file` system variable as
  `--mylite-primary-file`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_primary_file_path()` activates
  MyLite schema hooks only when `mylite_primary_file` is set.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_requested_engine_name()` records
  the SQL-requested engine name from `CREATE TABLE` while
  `@@enforce_storage_engine` makes MyLite the effective engine.
- `packages/libmylite/src/database.cc::runtime_arguments()` passes
  `--mylite-primary-file=<file>` for file-backed storage-engine builds and then
  configures sessions with `default_storage_engine=MYLITE`,
  `enforce_storage_engine=MYLITE`, and `use_stat_tables=NEVER`.
- `packages/libmylite/tests/embedded_handler_savepoint_test.cc` shows raw
  embedded MariaDB startup can use `--mylite-primary-file` outside the
  `libmylite` API when the primary file is created first.
- `mariadb/mysql-test/mariadb-test-run.pl` derives the bootstrap default engine
  from `--default-storage-engine`; the storage MTR mode should keep the MTR
  bootstrap default at `Aria` and set MyLite only inside the MyLite-owned test
  session.
- `mariadb/mysql-test/mariadb-test-run.pl` adds `loose-skip-plugin-*` for
  optional static plugins during bootstrap and generated config creation. The
  MyLite storage test therefore enables the static MyLite plugin from the
  test-specific `.opt` file, after bootstrap has created MariaDB's own Aria
  system tables.
- `mariadb/sql/handler.cc::ha_resolve_by_name()` maps supported engine aliases
  to MyLite only when a primary file is active and the session explicitly
  enforces MyLite. This prevents MariaDB bootstrap/system-table Aria reads from
  being routed into MyLite before the test session starts.
- `cmake/mariadb-mtr-smoke.cmake` is the current MTR profile and intentionally
  omits the static MyLite handler. A separate storage profile can include the
  MTR profile and add `PLUGIN_MYLITE_SE=STATIC`.

## Design

- Add a tiny first-party storage utility that creates an empty `.mylite` file
  through `mylite_storage_create_empty()`.
- Add `cmake/mariadb-mtr-storage-smoke.cmake`, including the baseline MTR smoke
  profile and enabling `PLUGIN_MYLITE_SE=STATIC`.
- Extend `tools/mylite-mtr-harness` with separate storage commands:
  - `list-storage` prints routed-storage MTR smoke tests.
  - `run-storage [suite.test...]` builds the storage MTR profile, prepares a
    fresh primary `.mylite` file, and runs exact selected tests.
  - `probe-storage suite.test...` mirrors `probe` against the storage profile.
- Keep the existing `list`, `run`, and `probe` commands baseline-oriented and
  unchanged in behavior.
- Add `mylite.routed_storage_engines` as the first storage-routed MTR test.
  Its `.opt` file enables the MyLite plugin and passes the storage primary file
  as embedded server arguments. The test then sets session
  `default_storage_engine=MYLITE` and `enforce_storage_engine=MYLITE`, creates
  representative engine forms, checks `SHOW CREATE TABLE` requested-engine
  metadata for each created form, inserts rows, and verifies `BLACKHOLE`
  discards while `MEMORY` / `HEAP` retain runtime rows.

## File Lifecycle

`run-storage` and `probe-storage` create a fresh primary file under the storage
MTR build directory before starting MTR. MTR owns temporary datadir state under
its `mysql-test/var` tree, while durable routed application state for the
MyLite-owned test is in that single primary `.mylite` file. The test does not
allow native InnoDB, MyISAM, Aria table files, or persistent `.frm` sidecars as
the final MyLite product model.

## Embedded Lifecycle And API

No public `libmylite` API change. This slice adds raw embedded MTR evidence for
the same storage-engine routing contract that `libmylite` configures for normal
file-backed sessions.

## Storage-Engine Routing Impact

The storage MTR mode is the first MTR entry point that requires
`PLUGIN_MYLITE_SE=STATIC` and an active `--mylite-primary-file`. The
compatibility claim remains narrow: MyLite-owned MTR smoke can route selected
engine aliases through MyLite storage; upstream MTR tests still need separate
normalization and unsupported-surface review before promotion.

## Build, Size, And Dependencies

No production dependency change. The storage MTR profile is opt-in and uses a
separate build tree, `build/mariadb-mtr-storage-smoke`. The storage init helper
links the existing `MyLite::storage` library and is built from the first-party
`dev` preset.

## Test Plan

- `cmake --preset dev`
- `cmake --build --preset dev --target mylite_storage_init`
- `tools/mylite-mtr-harness list-storage`
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_engines`
- `tools/mylite-mtr-harness run-storage`
- `tools/mylite-mtr-harness run mylite.bootstrap_schema`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Existing baseline MTR commands still target `build/mariadb-mtr-smoke` and do
  not enable MyLite storage routing.
- Storage MTR commands target `build/mariadb-mtr-storage-smoke`, require
  `PLUGIN_MYLITE_SE=STATIC`, and export the prepared primary-file path for
  MyLite-owned storage test option files.
- The storage primary file is created through MyLite storage code before MTR
  runs.
- Engine alias routing remains conditional on `@@enforce_storage_engine=MYLITE`
  so MariaDB bootstrap/system tables continue using their native bootstrap
  engine while MyLite sessions still route explicit aliases.
- `mylite.routed_storage_engines` reports an MTR pass under strict harness
  execution.
- Docs and compatibility claims clearly distinguish baseline MTR coverage from
  routed-storage MTR coverage.

## Verification Results

- `cmake --preset dev` passed.
- `cmake --build --preset dev --target mylite_storage_init` passed.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report` passed.
- `tools/mylite-mtr-harness list-storage` printed
  `mylite.routed_storage_engines`.
- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_engines`
  passed after expanding requested-engine metadata assertions.
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_engines` passed.
- `tools/mylite-mtr-harness run-storage` passed.
- `tools/mylite-mtr-harness run mylite.bootstrap_schema` passed.
- `tools/mylite-compat-harness run storage-engine` passed.

## Risks And Open Questions

- The storage MTR mode still starts MariaDB through `mariadb-test-run.pl`, not
  through `libmylite`, so it must remain a compatibility smoke rather than a
  public lifecycle replacement.
- Some upstream MTR tests set `default_storage_engine` or depend on native table
  files; promoting them to storage mode will need per-test review.
- MyLite-owned schema sidecar checks are covered by the follow-up
  `mtr-routed-storage-sidecar-smoke` slice; unrelated MTR bootstrap/system
  schemas still use native files and are intentionally outside that check.
