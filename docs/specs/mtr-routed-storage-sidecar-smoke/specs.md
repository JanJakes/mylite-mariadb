# MTR Routed Storage Sidecar Smoke

## Goal

Add MyLite-owned MTR coverage that proves routed storage tests do not create
durable MariaDB schema directories or native engine sidecars for the test
schema while executing through raw embedded MTR with an active primary
`.mylite` file.

## Non-Goals

- Scanning the whole MTR var tree. MariaDB bootstrap and MTR fixtures still use
  native `mysql`, `mtr`, `sys`, and `std_data` files.
- Replacing the first-party `compat-sidecar` CTest group.
- Claiming upstream MTR tests are safe to run against MyLite storage without
  per-test review.
- Adding new storage semantics beyond validating existing routed DDL/DML paths.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_db.cc::mysql_create_db_internal()` calls the MyLite schema
  hook path when schema hooks are active and no runtime schema directory exists.
- `mariadb/sql/sql_db.cc::mysql_create_mylite_db()` stores schema options
  through `mylite_schema_store_options()` instead of creating a filesystem
  database directory and `db.opt`.
- `mariadb/sql/sql_db.cc::mylite_runtime_schema_dir_exists()` intentionally
  lets an existing filesystem schema directory keep native behavior, so the MTR
  test must assert the MyLite-owned schema directory never appears.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_primary_file_path()` activates
  MyLite schema hooks only when `--mylite-primary-file` is set.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_schema_hook_store()` writes
  schema metadata to `mylite_storage_store_schema_definition()` in the primary
  file.
- `packages/libmylite/tests/embedded_storage_engine_test.c` already rejects
  durable sidecar suffixes such as `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`,
  `.MAD`, `aria_log*`, `ibdata*`, `ib_logfile*`, `undo*`, `mysql-bin*`, and
  `relay-log*` for first-party storage tests.
- `tools/mylite-mtr-harness run-storage` prepares a fresh primary file under
  `build/mariadb-mtr-storage-smoke` but MTR still bootstraps MariaDB system
  tables under `mysql-test/var`, so a useful MTR sidecar check must be scoped
  to the MyLite-owned schema and primary-file companions.

## Design

- Add `mylite.routed_storage_sidecars` to the storage MTR smoke list.
- Reuse the storage MTR profile and `.opt` arguments from
  `mylite.routed_storage_engines`.
- Add `suite/mylite/include/assert_no_routed_storage_sidecars.inc`.
  The include takes `mylite_sidecar_schema`, checks that the MyLite-owned schema
  directory is absent from both the active MTR datadir and the install template,
  and checks primary-file companions with the primary-file basename for native
  durable sidecar names.
- The test creates a MyLite-owned schema, forces `default_storage_engine=MYLITE`
  and `enforce_storage_engine=MYLITE`, runs representative omitted-engine,
  `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, `ENGINE=MEMORY`,
  `ENGINE=HEAP`, and `ENGINE=BLACKHOLE` DDL/DML plus ALTER, RENAME, and CTAS
  operations, then runs the sidecar include before and after cleanup.

## Compatibility Impact

The slice adds compatibility evidence rather than changing SQL behavior. It
strengthens the storage-routed MTR claim from "selected alias DDL/DML works" to
"selected alias DDL/DML works without publishing MariaDB-native durable schema
or table files for the MyLite-owned schema."

## Single-File And Lifecycle Impact

No file-format change. The MTR test uses the same primary file prepared by
`tools/mylite-mtr-harness run-storage`. The only accepted durable MyLite-owned
state is the primary `.mylite` file; transient MyLite journal companions remain
allowed by the storage layer, but native MariaDB durable sidecar names remain
forbidden for the routed schema.

## Public API, Build, Size, And Dependencies

No public API, production build, binary size, or dependency change. The Perl
sidecar assertion uses core Perl modules already required by MTR.

## Test Plan

- `tools/mylite-mtr-harness list-storage`
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_sidecars`
- `tools/mylite-mtr-harness run-storage`
- `tools/mylite-compat-harness run sidecar storage-engine`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `list-storage` includes the new sidecar smoke.
- The new MTR test passes under the storage profile.
- The sidecar include fails if the MyLite-owned schema appears as an MTR
  datadir/install-template directory.
- The sidecar include fails if a primary-file companion uses a native durable
  sidecar name.
- Docs and compatibility text explain that the MTR sidecar check is scoped to
  MyLite-owned schema state, not MariaDB's bootstrap/system fixtures.

## Verification Results

- `tools/mylite-mtr-harness list-storage` printed
  `mylite.routed_storage_engines` and `mylite.routed_storage_sidecars`.
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_sidecars`
  passed.
- `tools/mylite-mtr-harness run-storage` passed.
- `tools/mylite-compat-harness run sidecar storage-engine` passed.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
  passed.
- `find mariadb/mysql-test -name '*.reject' -print` printed nothing.
- `git diff --check` passed.

## Risks And Open Questions

- This does not scan unrelated MTR system schemas because those are expected to
  use native files during raw MTR bootstrap.
- Restart persistence remains better covered by first-party `libmylite`
  compatibility tests until a stable MTR restart pattern for MyLite-owned
  primary files is specified.
