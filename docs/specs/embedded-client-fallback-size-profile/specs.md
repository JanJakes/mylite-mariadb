# Embedded Client Fallback Size Profile

## Problem Statement

The aggressive MyLite minsize profile still links inherited embedded
Connector/C fallback paths around `mysql_real_connect()` and
`mysql_server_init()`: remote-client fallback, option-file defaults, client
plugin initialization, connection attributes, system username lookup, and
network port/socket defaults.

MyLite's core API opens a local `.mylite` file in-process. It does not expose a
daemon, remote host, password handshake, network socket, or client plugin
dialog. These fallback paths are useful for MariaDB's embedded C API
compatibility, but they are not needed by `libmylite` and are a prerequisite to
the larger direct-dispatch work.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/libmysqld/libmysql.c` implements
  `mysql_server_init()` and `mysql_server_end()`. It initializes client plugin
  support, checks service/env defaults for client port and socket globals, and
  then calls `init_embedded_server()` when built as `EMBEDDED_LIBRARY`.
- `vendor/mariadb/server/libmysqld/libmysqld.c` implements embedded
  `mysql_real_connect()`. It can fall back to `cli_mysql_real_connect()` for
  remote hosts, read option-file defaults with `mysql_read_default_options()`,
  read a system username with `read_user_name()`, and then create an embedded
  `THD`.
- `vendor/mariadb/server/libmysqld/lib_sql.cc` implements
  `check_embedded_connection()` and, when `NO_EMBEDDED_ACCESS_CHECKS` is set,
  transfers Connector/C connection attributes with `send_client_connect_attrs()`.
- `vendor/mariadb/server/include/my_global.h` defines
  `NO_EMBEDDED_ACCESS_CHECKS` for this build, so the minsize runtime already
  avoids server account authentication.
- The current linked smoke still contains `cli_mysql_real_connect`,
  `mysql_read_default_options`, `read_user_name`, `send_client_connect_attrs`,
  and client plugin init/deinit roots.

MariaDB documentation:

- MariaDB's embedded interface documents `libmysqld` as exposing the same C API
  shape as the normal client library.
- MariaDB Connector/C documents `MYSQL` as the client connection object and
  `mysql_real_connect()` as the client connection entry point.

## Scope

This slice may:

- add `MYLITE_DISABLE_EMBEDDED_CLIENT_FALLBACKS`,
- enable it in `tools/build-mariadb-minsize.sh`,
- keep local embedded `mysql_real_connect(mysql, nullptr, "root", ...)`
  working,
- reject remote-host embedded connects before `cli_mysql_real_connect()`,
- reject option-file defaults in the aggressive profile,
- avoid system username fallback by using an empty user when none is supplied,
- skip Connector/C connection attributes, and
- skip client plugin init/deinit and network default env/service lookup in
  `mysql_server_init()` / `mysql_server_end()`, and
- replace the client plugin loader entry points with no-plugin stubs in this
  profile.

## Non-Goals

This slice does not:

- remove the internal `MYSQL *` handle from `mylite_db`,
- remove `mysql_real_query()`, `mysql_store_result()`, `mysql_stmt_*()`, or
  prepared-statement client facades,
- replace result capture with direct `THD`/`Protocol` dispatch,
- remove `libmysql.c.o` or `client.c.o` from the archive,
- claim generic MariaDB embedded C API compatibility for the aggressive
  minsize profile, or
- change the public `libmylite` API or `.mylite` file format.

## Proposed Design

Add a `MYLITE_DISABLE_EMBEDDED_CLIENT_FALLBACKS` CMake option in
`libmysqld/CMakeLists.txt` and forward it to MariaDB-derived sources with
`ADD_DEFINITIONS()`.

When enabled:

- `mysql_server_init()` still performs the client-library initialization needed
  by the embedded runtime, but does not initialize client plugins or read
  network port/socket defaults from service/env state.
- `mysql_server_end()` skips client plugin deinitialization when plugin
  initialization was skipped.
- `client_plugin.c` compiles tiny no-plugin stubs for the retained public
  client plugin symbols instead of the loader, env-plugin, mutex, MEM_ROOT, and
  `dlopen()` implementation.
- embedded `mysql_real_connect()` returns `CR_CONN_UNKNOW_PROTOCOL` for remote
  host requests instead of calling `cli_mysql_real_connect()`.
- embedded `mysql_real_connect()` returns `CR_UNKNOWN_ERROR` when Connector/C
  option-file defaults are requested, because the aggressive MyLite profile
  starts the runtime from explicit arguments.
- embedded `mysql_real_connect()` uses an empty username when the caller does
  not provide one, avoiding OS account lookup. MyLite passes `"root"` today, so
  public MyLite behavior is unchanged.
- `check_embedded_connection()` skips connection-attribute transfer. MyLite does
  not expose connection attributes and this build has access checks disabled.

The open-close or bootstrap smoke should verify the local embedded connection
still works. A focused `#ifdef MYLITE_DISABLE_EMBEDDED_CLIENT_FALLBACKS` smoke
should verify a remote-host `mysql_real_connect()` attempt fails without trying
the network path.

## Affected Subsystems

- Embedded client/server bootstrap in `libmysql.c`.
- Embedded C API connect path in `libmysqld.c`.
- Embedded connection setup in `lib_sql.cc`.
- Aggressive minsize build configuration.
- Embedded bootstrap smoke and production size analysis.

## Single-File And Embedded-Lifecycle Impact

No file ownership, catalog, locking, recovery, or storage behavior changes. The
slice removes inherited network/client fallback behavior from the aggressive
profile and keeps the local embedded lifecycle used by MyLite.

## Public API Or File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

Compatibility impact: the aggressive profile no longer supports remote-host
fallback, option-file defaults, client plugin initialization, connection
attributes, or OS username fallback through MariaDB's embedded C API.
`libmylite` does not expose those surfaces.

## Binary-Size Impact

Expected linked savings are likely small-to-modest. This slice can remove
visible roots such as `cli_mysql_real_connect`, `mysql_read_default_options`,
`read_user_name`, client plugin init/deinit, and connection-attribute transfer
from the linked smoke. Static archive savings may be small because `libmysql.c`
and `client.c` still contain retained `mysql_*` and prepared-statement helpers
until direct dispatch removes those facades.

Implemented measurements against the preceding
`legacy-mysql500-collation-size-profile` baseline:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,515,034 | 25,504,414 | -10,620 |
| unstripped `mylite-open-close-smoke` | 6,616,792 | 6,585,232 | -31,560 |
| stripped `mylite-open-close-smoke` | 4,654,432 | 4,628,736 | -25,696 |

`llvm-size` total for the linked open-close smoke changed from 4,877,861 to
4,852,372 bytes (-25,489). The linked smoke no longer contains
`cli_mysql_real_connect`, `mysql_read_default_options`, `read_user_name`,
`send_client_connect_attrs`, `load_env_plugins`, or `getservbyname`. The
retained client plugin init/deinit symbols are tiny no-op stubs.

## License, Trademark, And Dependency Impact

No new dependency or license impact. This is a GPL-2.0-only MariaDB-derived
build-profile change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-embedded-bootstrap-smoke.sh
git diff --check
```

Measure:

- archive bytes,
- unstripped and stripped linked open-close smoke bytes,
- section profile changes, and
- absence or retention of the targeted fallback symbols in the linked smoke.

## Acceptance Criteria

- Passed: the minsize build succeeds with
  `MYLITE_DISABLE_EMBEDDED_CLIENT_FALLBACKS=ON`.
- Passed: embedded bootstrap, open/close, storage-engine, and compatibility
  smokes pass.
- Passed: local embedded `mysql_real_connect()` still works.
- Passed: a remote-host embedded `mysql_real_connect()` attempt fails before the
  inherited remote client path.
- Passed: size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-client-fallbacks \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-libmylite-open-close-smoke.sh tools/run-storage-engine-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The embedded bootstrap report includes:

```text
remote_connect_errno=2047
remote_connect_sqlstate=HY000
remote_connect_message=Wrong or unknown protocol
```

## Risks And Unresolved Questions

- This is not the full direct-dispatch win. It trims fallback paths while the
  public MyLite wrapper still uses internal `MYSQL *` and `MYSQL_STMT *`.
- Downstream users of the aggressive `libmariadbd.a` as a generic MariaDB
  embedded C API may observe lost remote/default-option/client-plugin behavior.
  That is acceptable only for the MyLite minsize profile.
- `client.c.o` may remain in the archive and linked runtime because
  `mysql_init_character_set()` and retained prepared-statement helpers still
  need client C API code.
