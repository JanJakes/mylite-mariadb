# Server Utility Function Trim

## Problem

The embedded profile keeps ordinary MySQL/MariaDB scalar functions, JSON,
GEOMETRY/GIS, collations, native storage, transactions, and diagnostics. It
does not need server-owned utility functions that depend on host filesystem
reads, connection-global named locks, replication position waits, benchmarking,
sleeping, or server-id based ID generation.

The current archive still builds native function builders and item
implementations for:

- `BENCHMARK()`
- `GET_LOCK()`
- `IS_FREE_LOCK()`
- `IS_USED_LOCK()`
- `RELEASE_ALL_LOCKS()`
- `RELEASE_LOCK()`
- `LOAD_FILE()`
- `MASTER_GTID_WAIT()`
- `MASTER_POS_WAIT()`
- `BINLOG_GTID_POS()`
- `SLEEP()`
- `UUID_SHORT()`

Those functions are server utility, server topology, or host-file surfaces. They
are not ordinary expression evaluation needed by application DDL/DML.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/item_create.cc` registers the function builders in
  `func_array[]` and constructs the corresponding `Item_*` classes.
- `mariadb/sql/item_func.cc` implements `BENCHMARK()`, `SLEEP()`, named-lock
  helpers, `MASTER_GTID_WAIT()`, `MASTER_POS_WAIT()`, and `UUID_SHORT()`.
- `mariadb/sql/item_strfunc.cc` implements `LOAD_FILE()` and
  `BINLOG_GTID_POS()`.
- `LOAD_FILE()` opens host files under `secure_file_priv` and is a filesystem
  read surface, not database-directory storage.
- `GET_LOCK()` and related named-lock helpers use server metadata locks scoped
  to server connection state, not MyLite directory locking.
- `MASTER_GTID_WAIT()`, `MASTER_POS_WAIT()`, and `BINLOG_GTID_POS()` belong to
  replication/binlog topology behavior already outside the embedded profile.
- `SLEEP()` and `BENCHMARK()` are testing/diagnostic utility functions that can
  stall an in-process caller without providing data behavior.
- `UUID_SHORT()` uses the inherited server-id/start-time counter. MyLite keeps
  `server_uuid_value()` for DDL-log internals, but does not need to expose
  `UUID_SHORT()` as SQL.

## Design

Add `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS`, defaulting to `ON` for
upstream-style embedded builds and forced `OFF` in the MyLite embedded
baseline.

When disabled:

- omit the listed builders from `item_create.cc`;
- omit the corresponding `Item_*` class definitions and method bodies;
- keep `item_func_sleep_init()` / `item_func_sleep_free()` as no-op lifecycle
  hooks so retained startup/cleanup callers stay simple;
- keep `uuid_short_init()` and `server_uuid_value()` because retained DDL-log
  internals use `server_uuid_value()`;
- reject direct and prepared calls before MariaDB dispatch through the
  `libmylite` server-surface policy.

## Affected MariaDB Subsystems

- Native function registry and function-builder classes.
- Scalar item implementations in `item_func.cc` and `item_strfunc.cc`.
- `libmylite` unsupported server-surface policy.

## Compatibility Impact

The omitted functions are explicitly unsupported server utility surfaces in the
default embedded profile. Ordinary scalar SQL functions such as `VERSION()`,
`FORMAT()`, string functions, date/time functions, JSON functions, GEOMETRY/GIS
functions, prepared statements, DDL/DML, native storage engines, transactions,
and public `libmylite` APIs remain in scope.

## Directory And Lifecycle Impact

No durable, temporary, lock, metadata, or runtime paths are added. Omitting
`LOAD_FILE()` removes a host-file read surface. Omitting named-lock SQL
functions does not change MyLite's database-directory advisory lock.

## Binary Size Impact

The current measured stripped archive after implementation is
26,077,728 bytes / 24.87 MiB with 693 archive members. This is 92,632 bytes
smaller than the previous 26,170,360-byte embedded archive, with no member
count change.

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

- `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS=OFF` appears in the embedded CMake
  cache.
- Direct and prepared calls to the omitted server utility functions fail through
  MyLite server-surface policy.
- Quoted string literals containing omitted function names remain ordinary SQL.
- Retained scalar utility functions such as `VERSION()` still execute.
- JSON, GEOMETRY/GIS, native storage, DDL/DML, transactions, and prepared
  statements still pass existing coverage.
