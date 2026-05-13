# Foreign Server Cache Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's foreign-server
metadata cache implementation from `sql_servers.cc`. MyLite already treats
foreign-server SQL as an unsupported server-oriented surface, and embedded
startup already initializes the cache without reading `mysql.servers`. The
remaining full cache implementation has no embedded value in the aggressive
profile.

Current baseline after `json-table-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,103,286 |
| `sql_servers.cc.o` object | 34,336 |
| stripped `mylite-open-close-smoke` | 5,730,200 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/unsupported-server-surface/specs.md` documents that
  `CREATE SERVER`, `ALTER SERVER`, and `DROP SERVER` are explicitly rejected in
  embedded builds with `ER_OPTION_PREVENTS_STATEMENT`.
- `docs/specs/foreign-server-cache-startup/specs.md` documents the later
  startup change that calls `servers_init(1)` for embedded builds, initializing
  cache structures without reading `mysql.servers`.
- `vendor/mariadb/server/sql/sql_servers.cc` implements the full
  `mysql.servers` cache, cache reload, create/alter/drop persistence, TDC
  invalidation for connection strings, and lookup by server name.
- `vendor/mariadb/server/sql/sql_parse.cc` includes `sql_servers.h`, but
  embedded builds reject create/alter/drop server commands before calling into
  `create_server()`, `alter_server()`, or `drop_server()`.
- `vendor/mariadb/server/sql/sql_show.cc:mysql_show_create_server()` calls
  `get_server_by_name()`; returning no match keeps `SHOW CREATE SERVER` on the
  missing-server diagnostic path.
- `vendor/mariadb/server/sql/sql_reload.cc` calls `servers_reload()` during
  reload operations. In the aggressive embedded profile, this can be a no-op.

## Scope

Add a minsize option that removes the full foreign-server cache implementation
from the embedded library. The option will:

- remove `../sql/sql_servers.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned foreign-server cache stub;
- keep startup/shutdown entry points as no-ops that succeed;
- keep reload as a no-op that succeeds;
- keep create/alter/drop server entry points returning the existing embedded
  option diagnostic if reached; and
- keep lookup returning no server.

## Non-Goals

- Do not implement foreign-server metadata.
- Do not create or emulate `mysql.servers`.
- Do not change parser support for foreign-server syntax.
- Do not change non-embedded MariaDB behavior.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_FOREIGN_SERVER_CACHE` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_foreign_server_stub.cc`.
`servers_init()`, `servers_reload()`, and `servers_free()` will preserve the
startup/shutdown call contract without allocating the inherited cache. Lookup
will return `nullptr`. `create_server()`, `alter_server()`, and
`drop_server()` will set `ER_OPTION_PREVENTS_STATEMENT` and return an error
code defensively, although normal embedded command dispatch rejects those
commands before reaching the stub.

## Affected Subsystems

- Embedded minsize SQL source list.
- Foreign-server cache startup and reload hooks.
- `SHOW CREATE SERVER` missing-server behavior.
- Binary-size documentation.

## DDL Metadata Routing Impact

No supported MyLite table DDL changes. Foreign-server metadata remains an
unsupported server metadata surface.

## Single-File And Embedded-Lifecycle Impact

This removes the remaining embedded foreign-server cache allocation and any
remaining possibility of the minsize profile reading or mutating
`mysql.servers` through the cache implementation.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Measured on top of `json-table-size-profile`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,103,286 | 30,072,536 | -30,750 |
| `mylite/mylite-open-close-smoke` | 7,964,712 | 7,960,568 | -4,144 |
| stripped `mylite-open-close-smoke` copy | 5,730,200 | 5,726,568 | -3,632 |

The embedded archive still contains 422 objects. It now contains the
3,832-byte `mylite_foreign_server_stub.cc.o` member instead of
`sql_servers.cc.o`. The linked smoke binary retains only the small stub
symbols for `servers_init()`, `servers_reload()`, `servers_free()`, and
`get_server_by_name()`, with no linked `servers_cache` or `THR_LOCK_servers`
symbols found.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-foreign-server-cache \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-foreign-server-cache \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-foreign-server-cache \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-foreign-server-cache \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `sql_servers.cc.o` in `libmariadbd.a`;
- presence and size of the replacement stub; and
- absence of linked full cache symbols such as `servers_cache`.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- Foreign-server SQL remains explicitly rejected in embedded bootstrap smoke.
- The embedded archive no longer contains `sql_servers.cc.o`.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification Results

All acceptance checks passed in
`MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-foreign-server-cache`.

The embedded bootstrap smoke reported `status=0`, kept the
`create_server`, `alter_server`, and `drop_server` rejection checks, and
reported `mysql_servers_startup=absent`. The open/close smoke and
compatibility harness both reported `status=0`.

## Risks And Unresolved Questions

- `FLUSH` paths that call `servers_reload()` now silently skip foreign-server
  reload work in the aggressive embedded profile. That is acceptable while
  foreign-server metadata is unsupported.
- A future profile that supports foreign servers or `mysql.servers` must disable
  this size option.
