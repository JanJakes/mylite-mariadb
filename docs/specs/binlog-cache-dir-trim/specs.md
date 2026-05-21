# Binlog Cache Directory Trim

## Problem

The embedded baseline starts with binary logging disabled and builds with
`MYLITE_WITH_BINLOG_CORE=OFF`, but MariaDB startup still calls
`init_binlog_cache_dir()`. In upstream MariaDB this helper builds the inherited
`#binlog_cache_files` path, scans or deletes the directory when binlogging is
off, and creates it when binlogging is on.

MyLite's no-binlog embedded profile does not write binary-log cache files. The
startup directory scan/delete step is daemon cleanup work, not application SQL,
native storage, transaction, JSON, GEOMETRY/GIS, or public C API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/mysqld.cc` calls `init_binlog_cache_dir()` during startup after
  optional binary-log open.
- `mariadb/sql/log_cache.cc` defines `BINLOG_CACHE_DIR` as
  `#binlog_cache_files` and implements the inherited directory setup and
  cleanup logic.
- `mariadb/sql/log.cc` opens statement and transaction binlog caches using
  `binlog_cache_dir` when binary logging is active.
- `packages/libmylite/src/database.cc` passes `--skip-log-bin`, and the
  embedded baseline sets `MYLITE_WITH_BINLOG_CORE=OFF`.

## Design

Reuse the existing `MYLITE_WITH_BINLOG_CORE` boundary. When the binary-log core
is disabled:

- keep the `init_binlog_cache_dir()` link contract;
- zero `binlog_cache_dir` and return success without building, scanning,
  deleting, or creating `#binlog_cache_files`;
- omit the `#binlog_cache_files` string from the embedded archive;
- keep the upstream directory setup when `MYLITE_WITH_BINLOG_CORE=ON`.

Add server-surface coverage that no `#binlog_cache_files` companion appears in
the MyLite database `datadir/`.

## Compatibility Impact

No public `libmylite` API change. The default embedded profile already rejects
replication and binlog command families and reports `@@log_bin=0`. Custom
profiles that re-enable binary logging keep the upstream directory behavior by
building with `MYLITE_WITH_BINLOG_CORE=ON`.

## Directory And Storage Impact

No MyLite-created file format changes. The default no-binlog profile stops
performing inherited cleanup for a daemon-owned cache directory that MyLite
does not create.

## Binary Size Impact

On this branch, `tools/mariadb-embedded-build all` reduced the stripped archive
from 26,267,304 bytes / 25.05 MiB to 26,265,424 bytes / 25.05 MiB with the
member count unchanged at 698. The pre-strip archive moved from 26,831,128
bytes to 26,829,192 bytes.

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

- `MYLITE_WITH_BINLOG_CORE=OFF` still appears in the embedded CMake cache.
- `#binlog_cache_files` is absent from the stripped embedded archive.
- Server-surface tests confirm the binlog cache directory is absent under
  `datadir/`.
- Supported SQL, native storage, transaction, prepared-statement, and
  directory-lifecycle coverage still pass.
