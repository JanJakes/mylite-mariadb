# Disabled Startup Option Trim

## Problem

The embedded profile has already disabled or rejected several daemon-owned
subsystems, but `mariadb/sql/mysqld.cc` still registers startup option rows for
some of their configuration knobs. These rows are dead weight in the default
`libmylite` startup path and can imply configurable server behavior that the
embedded core does not support.

The safe trim is narrow: remove only options for disabled binary-log,
replication, and dynamic-plugin-loading surfaces that MyLite itself does not
use for startup. Keep the retained serverless options that define the embedded
contract, including `--skip-log-bin`, `--skip-slave-start`, `--plugin-dir`, and
directory-owned storage options.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` starts the embedded runtime with
  `--skip-log-bin`, `--skip-slave-start`, `--skip-grant-tables`,
  `--skip-networking`, `--plugin-dir=<db>/run/plugins`, and storage-directory
  options. These options must remain accepted.
- `mariadb/sql/mysqld.cc` registers `my_long_options[]` rows for binary-log
  and replication topology knobs such as `--flashback`,
  `--gtid-pos-auto-engines`, `--log-bin-index`, `--relay-log-index`,
  `--master-info-file`, `--master-retry-count`, and `--show-slave-auth-info`.
- `mariadb/sql/mysqld.cc` also registers `--plugin-load` and
  `--plugin-load-add`, even when the embedded baseline sets
  `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF`.
- `docs/specs/replication-filter-trim/specs.md` already removes the
  corresponding replication and binlog filter options when
  `MYLITE_WITH_REPLICATION_FILTERS=OFF`.

## Design

Add `MYLITE_WITH_DISABLED_STARTUP_OPTIONS`, defaulting to `ON` for normal
MariaDB-style builds and forced `OFF` in the MyLite embedded baseline.

When disabled, omit startup option rows for disabled topology and dynamic
plugin-loading surfaces that are not required by MyLite startup:

- `--flashback`,
- `--gtid-pos-auto-engines`,
- `--log-bin-index`,
- `--relay-log-index`,
- `--master-info-file`,
- `--master-retry-count`,
- `--show-slave-auth-info`,
- `--plugin-load`,
- `--plugin-load-add`.

Keep `--skip-log-bin`, `--skip-slave-start`, `--plugin-dir`, `--log-output`,
temporary-directory, storage-directory, and other retained startup options.
Also keep static built-in plugin registration and native storage engines.

## Compatibility Impact

No public `libmylite` API changes. The default MyLite API does not accept
arbitrary MariaDB startup options, and its fixed startup vector keeps using
only retained options.

Custom MariaDB-style embedded profiles can keep the inherited option rows by
leaving `MYLITE_WITH_DISABLED_STARTUP_OPTIONS=ON`.

## Directory And Storage Impact

No file-format, metadata, native-storage, transaction, or directory-layout
change. The trim only removes parser rows for startup options that would
configure disabled server surfaces.

## Binary Size Impact

On this branch, `tools/mariadb-embedded-build all` reduced the stripped archive
from 26,269,664 bytes / 25.05 MiB to 26,267,304 bytes / 25.05 MiB with the
member count unchanged at 698. The pre-strip archive moved from 26,833,536
bytes to 26,831,128 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_DISABLED_STARTUP_OPTIONS=OFF` appears in the embedded CMake
  cache.
- The default embedded runtime still opens and closes databases through
  `libmylite`.
- The stripped archive no longer contains option-only strings such as
  `plugin-load`, `plugin-load-add`, `master-info-file`, and
  `show-slave-auth-info`.
- Static built-in plugins and native storage-engine coverage continue to pass.
