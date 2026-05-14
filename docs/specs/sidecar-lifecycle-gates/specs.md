# Sidecar Lifecycle Gates

## Problem Statement

MyLite now stores explicit `ENGINE=MYLITE` table-definition metadata in the
primary `.mylite` catalog, but the tests only lightly check that the primary
directory is clean after close. The project needs stronger gates before engine
routing aliases `InnoDB`, `MyISAM`, `Aria`, or omitted-engine DDL to MyLite.

This slice adds repeatable checks that fail when metadata DDL, failed create
paths, unsupported catalog-changing DDL, close/reopen, or runtime cleanup leave
MariaDB-owned durable sidecars outside the MyLite file lifecycle.

## Scope

- Define the first forbidden sidecar name and extension set.
- Add reusable test helpers for checking the primary-file directory and runtime
  root after metadata DDL and final close.
- Add catalog checks that failed duplicate create and unsupported DDL do not
  publish extra catalog records.
- Keep the current MyLite-owned temporary runtime directory model, but require
  it to be empty after the final close.
- Update compatibility and roadmap status when the gates land.

## Non-Goals

- Do not implement `DROP`, `RENAME`, `ALTER`, row storage, engine routing, crash
  recovery, file locks, or MyLite-owned journal/WAL companions.
- Do not ban all files while the embedded runtime is open. MariaDB may create
  temporary bootstrap files under the MyLite-owned runtime directory until later
  slices remove or replace them.
- Do not claim crash-clean recovery companions yet. This slice only covers
  clean close and failed statement cleanup.
- Do not inspect arbitrary external directories outside the test-owned primary
  root and runtime root.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/handler.h:1621-1687` defines table discovery and existence
  callbacks; MyLite implements these so metadata does not depend on durable
  `.frm` files.
- `mariadb/sql/handler.cc:5350-5402` shows the default `delete_table()` and
  `rename_table()` implementations operate on `bas_ext()` file extensions.
  MyLite must not fall back to those file-extension paths for catalog-owned
  tables.
- `mariadb/sql/handler.cc:6488-6512` suppresses `.frm` writes when the storage
  engine implements discovery.
- `mariadb/sql/table.cc:1809-1872` shows
  `TABLE_SHARE::init_from_binary_frm_image()` writes a `.frm` file only when
  its `write` argument is true.
- `mariadb/storage/mylite/ha_mylite.cc` currently sets no table-file
  extensions, implements MyLite discovery, stores binary table definitions in
  the primary file, and rejects `DROP`/`RENAME`.

## Proposed Design

The first gate should live in the storage-engine smoke tests because it depends
on the static MyLite handler build. The test helper should recursively scan the
test-owned root after final close and fail on known MariaDB-owned durable
sidecars:

- `.frm`, `.par`;
- `.ibd`, `ibdata*`, `ib_logfile*`, `undo*`;
- `.MYD`, `.MYI`;
- `.MAI`, `.MAD`, `aria_log*`;
- binlog and relay-log names.

The helper should allow the primary `.mylite` file and the empty MyLite-owned
runtime root. It should not treat future MyLite-owned companions as valid until
their names and lifecycle are documented in a later recovery or locking slice.

Failed create cleanup should be covered through observable metadata behavior:
duplicate explicit MyLite create must fail, and the catalog must still list one
table record. Unsupported `DROP TABLE` must fail and must not remove or duplicate
the catalog entry.

## Affected MariaDB Subsystems

- MyLite storage-engine handler under `mariadb/storage/mylite/`.
- MariaDB table discovery through `discover_table`,
  `discover_table_names`, and `discover_table_existence`.
- MariaDB file-extension fallback behavior through `handler::delete_table()` and
  `handler::rename_table()`.
- Embedded runtime startup and cleanup through `libmylite`.

## Compatibility Impact

The compatibility claim remains partial. `ENGINE=MYLITE` metadata can be
created and rediscovered, but row DML and catalog-changing DDL are unsupported.
The new claim is that the covered metadata path does not leave known MariaDB
durable sidecars after clean close.

## Implementation Status

Implemented. The storage-engine smoke tests now scan the test-owned primary
root recursively for known MariaDB durable sidecars, require the MyLite-owned
runtime root to be empty after close, and count catalog table records after
failed duplicate create and unsupported drop.

## Single-File And Embedded Lifecycle

The primary `.mylite` file remains the only durable database asset covered by
this slice. The temporary runtime directory remains MyLite-owned bootstrap debt
and must be empty after final close in tests.

## Public API Or File-Format Impact

No new public C API or file-format record type is required. The tests exercise
existing open/close, direct SQL execution, and catalog table-listing APIs.

## Storage-Engine Routing Impact

No engine aliases are added. These gates are a prerequisite for routing omitted
engine, `InnoDB`, `MyISAM`, or `Aria` DDL to MyLite because those aliases would
otherwise risk silently creating legacy engine sidecars.

## Wire Protocol Or Integration Impact

None directly. Future protocol integrations should inherit the same sidecar
behavior through `libmylite`.

## Binary Size Impact

No production dependency or MariaDB component is added. Test-only helper code
does not change the embedded archive size.

## License Or Dependency Impact

No new dependency is introduced.

## Test And Verification Plan

1. Extend the storage-engine smoke test with recursive forbidden-sidecar
   detection after metadata DDL, failed duplicate create, unsupported drop, and
   close/reopen.
2. Count catalog table entries after failed duplicate create and unsupported
   drop to prove failed paths do not publish extra records.
3. Keep the `:memory:` discovery regression so MyLite discovery without a
   primary file behaves as an empty engine.
4. Run dev, embedded, and storage-smoke presets plus format, format-check,
   clang-tidy, and `git diff --check`.

## Acceptance Criteria

- Metadata DDL and reopen tests fail on known MariaDB durable sidecars in the
  test-owned primary root.
- The runtime root is empty after final close.
- Failed duplicate create and unsupported drop leave exactly one catalog table
  record.
- Existing storage, embedded, and storage-smoke tests pass.
- Compatibility and roadmap docs describe the gate without claiming row storage,
  engine aliasing, crash recovery, or catalog-changing DDL.

## Risks And Unresolved Questions

- The forbidden-name list is intentionally conservative and may need expansion
  as more MariaDB surfaces are enabled.
- This slice does not prove crash cleanup. Recovery companions need a separate
  design with simulated crash tests.
- Once MyLite-owned journals, WAL files, shared-memory files, or lock files are
  added, this helper must distinguish documented MyLite companions from
  forbidden MariaDB-owned sidecars.
