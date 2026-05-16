# Native InnoDB Engine Trim

## Problem

The default embedded profile still statically registers native InnoDB and
merges InnoDB archive members into `libmariadbd.a`. MyLite already starts
embedded sessions with InnoDB disabled at runtime and routes file-backed
application `ENGINE=InnoDB` DDL to the MyLite storage engine, so native InnoDB
tablespaces, redo, undo, dictionary, and background runtime are not valid
durable application storage for the product.

The remaining work is to remove native InnoDB from the MyLite embedded build
profile while preserving:

- embedded runtime startup,
- MariaDB SQL semantics above the handler boundary,
- routed `ENGINE=InnoDB` application DDL/DML through MyLite storage,
- sidecar gates for InnoDB-owned files,
- the opt-in MTR smoke profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/CMakeLists.txt` registers `innobase` through
  `MYSQL_ADD_PLUGIN(innobase ... STORAGE_ENGINE DEFAULT
  RECOMPILE_FOR_EMBEDDED ...)`.
- The pre-slice embedded CMake cache had `PLUGIN_INNOBASE=STATIC`,
  `WITH_INNOBASE_STORAGE_ENGINE=ON`, and `EMBEDDED_PLUGIN_LIBS` contains
  `innobase`.
- The pre-slice default embedded archive contained InnoDB handler and runtime
  objects such as `ha_innodb.cc.o`, `handler0alter.cc.o`, `srv0start.cc.o`,
  `trx0trx.cc.o`, `row0mysql.cc.o`, `dict0dict.cc.o`, `buf0buf.cc.o`,
  `fil0fil.cc.o`, `fsp0fsp.cc.o`, and `log0log.cc.o`.
- A probe build with `PLUGIN_INNOBASE=NO` completed without new source stubs.
  Its embedded archive measured 21,020,816 bytes / 20.05 MiB with 481 members,
  versus the pre-slice 25,996,816 bytes / 24.79 MiB with 596 members.
- The same probe confirmed `EMBEDDED_PLUGIN_LIBS` no longer contains
  `innobase`, and archive member probes showed no native InnoDB runtime object
  names matching the handler, buffer, dictionary, file, tablespace, log, row,
  server, transaction, or B-tree prefixes.
- `packages/libmylite/src/database.cc` and the raw embedded comparison test
  pass `--innodb=OFF` today. When native InnoDB is absent, embedded startup
  rejects that option as ambiguous because other `innodb-*` options remain.
  `--loose-innodb=OFF` is ambiguous for the same reason, so the startup option
  must be conditional on the referenced embedded archive registering native
  InnoDB.
- `tools/mylite-mtr-harness` also passes `--mysqld=--innodb=OFF`; because the
  MTR smoke profile now requires `PLUGIN_INNOBASE=NO`, the MTR runner can omit
  that option entirely.
- `mariadb/sql/mysqld.cc` already falls back to `Aria` as the compiled default
  storage engine when `WITH_INNOBASE_STORAGE_ENGINE` is not defined and native
  MyISAM is disabled.
- `docs/specs/engine-routing-policy/specs.md` and storage-smoke tests route
  explicit `ENGINE=InnoDB` requests to effective `MYLITE` storage for
  file-backed sessions.

## Design

- Force `PLUGIN_INNOBASE=NO` in the MyLite embedded baseline.
- Disable InnoDB-only tuning options in the MyLite baseline where they are
  otherwise still reported by CMake feature output despite the plugin being
  disabled.
- Detect `PLUGIN_INNOBASE=NO` in referenced embedded build trees and compile
  first-party embedded targets with `MYLITE_MARIADB_HAS_INNOBASE=0`.
- Keep `--innodb=OFF` only when the referenced embedded archive registers
  native InnoDB. Omit the option when native InnoDB is absent.
- Remove the MTR smoke runner's explicit InnoDB startup option because the MTR
  smoke build requires `PLUGIN_INNOBASE=NO`.
- Extend MTR profile validation to require `PLUGIN_INNOBASE=NO`.
- Extend server-surface coverage so the default embedded profile proves
  `SHOW ENGINES` does not advertise native `InnoDB`.
- Keep routed `ENGINE=InnoDB` behavior unchanged for file-backed storage-smoke
  sessions: requested engine remains `InnoDB`, effective engine remains
  `MYLITE`, and no InnoDB durable sidecars are created.

## Affected Subsystems

- MariaDB embedded CMake baseline.
- Embedded runtime startup arguments in `libmylite`.
- Raw embedded SQL comparison startup arguments.
- MTR smoke harness startup and profile validation.
- Server-surface tests and compatibility documentation.
- Size-profile measurement documentation.

## MySQL/MariaDB Compatibility Impact

Native InnoDB is not registered in the default embedded profile. `SHOW ENGINES`
should not advertise `InnoDB`, and native InnoDB system tables, information
schema plugin tables, tablespaces, redo/undo, and native crash recovery are not
available through the embedded core profile.

Application DDL that says `ENGINE=InnoDB` remains supported through MyLite
routing in file-backed storage builds. This is a deliberate compatibility
shape: MyLite accepts common InnoDB table definitions where its storage layer
can provide the requested behavior, while explicit unsupported InnoDB-specific
semantics such as foreign keys, native tablespaces, native fulltext/spatial
access paths, and full InnoDB transaction isolation remain unsupported or
planned.

## DDL Metadata Routing Impact

No catalog format change. Existing requested-engine/effective-engine metadata
continues to record `InnoDB` requests as effective `MYLITE` tables.

## Single-File And Embedded-Lifecycle Impact

The disabled profile must not create InnoDB `.ibd`, redo, undo, or system
tablespace files as durable application storage. Existing sidecar gates remain
the product invariant for routed `ENGINE=InnoDB` DDL/DML.

Conditional startup arguments preserve compatibility with older
InnoDB-present embedded build trees while allowing the default InnoDB-absent
profile to start without ambiguous `innodb` option parsing.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Routed `ENGINE=InnoDB` remains a supported requested-engine spelling in
file-backed MyLite sessions. Native `InnoDB` should no longer appear in the
default embedded profile's `SHOW ENGINES` output.

## Binary-Size Impact

The implemented default embedded profile measures 21,020,816 bytes / 20.05 MiB
with 481 archive members, a reduction of 4,976,000 bytes and 115 members from
the previous 25,996,816-byte / 596-member profile.

The opt-in storage-smoke profile measures 21,216,208 bytes / 20.23 MiB with
484 archive members after the same trim.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived native InnoDB objects
from the disabled embedded profile.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the storage-smoke profile with `PLUGIN_MYLITE_SE=STATIC`.
- Confirm `libmariadbd.a` omits native InnoDB archive members such as
  `ha_innodb.cc.o`, `handler0alter.cc.o`, `srv0start.cc.o`, `trx0trx.cc.o`,
  `row0mysql.cc.o`, `dict0dict.cc.o`, `buf0buf.cc.o`, `fil0fil.cc.o`,
  `fsp0fsp.cc.o`, and `log0log.cc.o`.
- Confirm `SHOW ENGINES` in the default embedded profile does not report
  `InnoDB`.
- Confirm routed storage-smoke `ENGINE=InnoDB` DDL/DML and catalog metadata
  still pass.
- Confirm sidecar gates still detect no InnoDB-owned durable application files.
- Run the opt-in MTR smoke profile.
- Run dev, embedded, and storage-smoke CTest presets.
- Run relevant compatibility harness reports, size report, format checks, tidy,
  shell syntax checks, and `git diff --check`.

## Verification Results

- `tools/mariadb-embedded-build all`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- Native InnoDB archive-member probes against the default and storage-smoke
  archives.
- No-InnoDB targeted embedded open/close and exec CTest run against
  `build/mariadb-no-innodb-probe`.
- `cmake --preset dev`, `cmake --preset embedded-dev`, and
  `cmake --preset storage-smoke-dev`
- `cmake --build --preset dev`, `cmake --build --preset embedded-dev`, and
  `cmake --build --preset storage-smoke-dev`
- `ctest --preset dev --output-on-failure`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface storage-engine sidecar routed-ddl-dml`
- `tools/mylite-mtr-harness run`
- `tools/mylite-size-report`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-mtr-harness tools/mylite-size-report`
- `git diff --check`
- `find mariadb/mysql-test -name '*.reject' -print`

## Acceptance Criteria

- Default embedded and storage-smoke archives omit native InnoDB engine objects.
- Embedded startup succeeds with native InnoDB absent.
- The opt-in MTR smoke profile starts and passes without native InnoDB.
- File-backed routed `ENGINE=InnoDB` behavior remains covered and unchanged.
- Documentation records the unsupported native-engine boundary and measured
  size impact.

## Risks And Open Questions

- Some retained SQL variables or parser paths mention InnoDB generically even
  when native InnoDB is absent. This slice should not chase every mention; it
  should only remove the native storage engine and startup dependency while
  leaving generic SQL compatibility variables alone unless tests prove they are
  misleading or harmful.
- Broader InnoDB-specific information-schema and status surfaces may need a
  later server-surface policy slice if they remain visible without the plugin.
- `PLUGIN_INNOBASE=NO` is a large trim; the review gate should be strict about
  routed `ENGINE=InnoDB` coverage, MTR smoke, and sidecar checks before
  accepting the baseline change.
