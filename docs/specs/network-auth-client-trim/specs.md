# Network Auth Client Trim

## Problem Statement

The MyLite embedded profile opens a local database directory in-process and
does not perform a client/server authentication plugin handshake. The default
embedded archive still linked MariaDB's inherited client authentication plugin
descriptors and plugin VIO negotiation helpers for remote
`mysql_real_connect()` and `mysql_change_user()` paths.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql-common/client.c` declares the built-in client authentication
  plugin descriptors for `mysql_native_password` and `mysql_old_password`.
- The same source implements the plugin VIO dialog helpers and
  `run_plugin_auth()` used by remote client authentication and inherited
  `mysql_change_user()`.
- `mariadb/libmysqld/libmysqld.c` uses `check_embedded_connection()` for the
  local embedded `mysql_real_connect()` path. That path does not use
  `run_plugin_auth()`.
- `mariadb/libmysqld/lib_sql.cc` still uses `send_client_connect_attrs()` for
  embedded connection attributes, and server authentication code still needs
  the shared `mpvio_info()` helper. Those helpers must stay linked.

## Proposed Design

Add `MYLITE_WITH_NETWORK_AUTH_CLIENT`, defaulting to `ON` for upstream-style
builds and forced `OFF` in the MyLite embedded baseline. When disabled:

- `mysql_client_builtins` is an empty built-in client plugin list.
- Client authentication plugin descriptors and plugin VIO dialog helpers are
  not compiled into `client.c.o`.
- `run_plugin_auth()` remains as a small fail-closed stub for inherited raw
  remote client and `mysql_change_user()` paths.
- `send_client_connect_attrs()` and `mpvio_info()` remain available for the
  retained embedded connection path and server-side helpers.

## Compatibility Impact

No supported `libmylite` behavior is removed. The primary API opens a local
database directory without server users, sockets, or an authentication plugin
negotiation. Raw inherited MariaDB C API paths that require network
client-auth negotiation fail closed in the default embedded archive.

Native storage, ordinary SQL execution, prepared statements, JSON,
GEOMETRY/GIS, DDL, DML, transactions, and the public MyLite API are not
affected.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,047,312 bytes / 24.84 MiB with 693 members, down 6,024 stripped bytes from
the prior 26,053,336-byte embedded profile.

The pre-strip archive moved from 26,614,200 bytes to 26,607,496 bytes.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_NETWORK_AUTH_CLIENT=OFF` appears in the embedded CMake
  cache.
- Confirm `client.c.o` no longer contains the client auth descriptor strings
  or plugin VIO helper symbols.
- Confirm `run_plugin_auth()` remains linked as the fail-closed backstop for
  inherited raw client paths.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The default embedded archive omits inherited network client authentication
  plugin handshake support.
- The local embedded `libmylite` open path remains covered and unchanged.
- Server-side auth helper code required by retained MariaDB internals remains
  linked.
- Unsupported raw remote client auth paths fail closed.
