# Storage Engine Skeleton

## Goal

Add the first MyLite storage boundaries:

- an internal first-party storage package that will own single-file catalog,
  page, transaction, lock, and recovery code;
- an opt-in MariaDB storage-engine handler skeleton under `mariadb/storage/`
  that proves the registration path without claiming table storage behavior.

The skeleton should make future storage work easier to land in small slices. It
must not present stubbed DDL or DML as supported MyLite storage.

## Non-Goals

- Do not implement `.mylite` file format, catalog pages, row storage, indexes,
  transactions, recovery, or locking.
- Do not route `ENGINE=InnoDB`, `ENGINE=MyISAM`, or `ENGINE=Aria` to MyLite.
- Do not enable the MyLite MariaDB handler in the default embedded baseline.
- Do not create durable MariaDB sidecar files for MyLite tables.
- Do not expose the storage package as public ABI.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48642ed6d21e54668641d5f31475f62fa0e`.
- `mariadb/cmake/plugin.cmake:35` defines `MYSQL_ADD_PLUGIN()`. Lines 111-115
  derive the `WITH_<PLUGIN>_STORAGE_ENGINE` variable for storage engines.
- `mariadb/cmake/plugin.cmake:147` creates static plugin libraries. Lines
  151-167 add embedded-library variants when `WITH_EMBEDDED_SERVER` is enabled,
  and lines 185-188 append static plugins to `EMBEDDED_PLUGIN_LIBS`.
- `mariadb/cmake/plugin.cmake:320` implements `CONFIGURE_PLUGINS()`, which
  globs `storage/*` and adds each directory with a `CMakeLists.txt`.
- `mariadb/storage/example/CMakeLists.txt:16` shows the storage-engine plugin
  registration shape with `MYSQL_ADD_PLUGIN(... STORAGE_ENGINE ...)`.
- `mariadb/storage/example/ha_example.cc:245` initializes a handlerton and sets
  `handlerton::create` at line 252. Lines 291-300 allocate the handler.
- `mariadb/storage/example/ha_example.h:185` lists required handler methods:
  `open()`, `close()`, `rnd_init()`, `rnd_next()`, `rnd_pos()`, `position()`,
  `info()`, `external_lock()`, `create()`, and `store_lock()`.
- `mariadb/storage/example/ha_example.cc:1088` declares the plugin entry that
  controls the engine name shown to MariaDB.
- `mariadb/sql/handler.h:5228`, `mariadb/sql/handler.h:5239`,
  `mariadb/sql/handler.h:5320`, and `mariadb/sql/handler.h:5445` mark core
  handler methods as pure virtual, so even a registration skeleton must define
  them.

## Design

The internal storage package lives under `packages/mylite-storage/` and builds
as `MyLite::storage`. It is a first-party target, not a public installed API.
The initial package exposes only skeleton capabilities:

- engine name: `MYLITE`;
- format version: `0`, meaning no durable file format exists yet;
- a `STUB` capability flag to prevent accidental interpretation as a real
  storage implementation.

MariaDB-facing glue lives under `mariadb/storage/mylite/`. The CMake plugin name
is `mylite_se` to avoid colliding with the first-party `libmylite` CMake target,
while the storage-engine name reported to MariaDB is `MYLITE`.

The handler skeleton is disabled by default:

```sh
-DPLUGIN_MYLITE_SE=STATIC
```

is required to compile and register it in a MariaDB build. This keeps the
minimal embedded build baseline stable until a later slice adds handler smoke
tests and records the size delta.

The first handler behavior should be deliberately conservative:

- `open()` and `close()` manage only MariaDB handler lock scaffolding.
- scans return end-of-file immediately.
- positional reads and table creation return unsupported-command errors.
- no file extensions are advertised, so MariaDB's default file cleanup does not
  imply durable MyLite sidecars.

## Compatibility Impact

The default build and runtime compatibility surface do not change because the
MariaDB handler is opt-in and disabled by default. When enabled manually, the
engine may appear as `MYLITE`, but it must not be treated as a supported storage
engine yet.

The storage package does not change SQL behavior or public `libmylite` ABI.
Later slices that enable DDL, DML, engine routing, or default registration must
update `docs/COMPATIBILITY.md` and the baseline size evidence.

## Test Plan

1. Build the first-party `mylite_storage` target.
2. Run the first-party storage capabilities test.
3. Configure a separate MariaDB build with `-DPLUGIN_MYLITE_SE=STATIC`.
4. Build the opt-in MariaDB `mylite_se` target to prove the handler compiles.
5. Do not claim SQL smoke coverage until the embedded bootstrap and execution
   APIs can run controlled statements.

## Acceptance Criteria

- `packages/mylite-storage/` exists and is built by the first-party CMake
  preset.
- The storage package has focused tests proving only skeleton capabilities.
- `mariadb/storage/mylite/` contains an opt-in storage-engine handler skeleton.
- The opt-in MariaDB handler target compiles with the embedded build profile.
- The default MariaDB embedded baseline remains unchanged.

## Risks And Open Questions

- Handler coverage is compile-only until the embedded bootstrap and SQL
  execution APIs can run controlled smoke statements.
- The MariaDB handler is not linked to `MyLite::storage` yet; that boundary
  should be connected only when real storage behavior exists.
- Enabling DDL or routing existing MariaDB engines to MyLite requires explicit
  compatibility and file-lifecycle documentation.
