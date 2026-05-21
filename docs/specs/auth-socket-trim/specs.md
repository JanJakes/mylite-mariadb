# Auth Socket Trim

## Problem Statement

The MyLite embedded profile opens a local database directory in-process and
does not perform network or Unix socket client authentication. The default
embedded archive still linked MariaDB's `unix_socket` server authentication
plugin.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/plugin/auth_socket/auth_socket.c` declares the `unix_socket`
  authentication plugin for socket-client login.
- The plugin target is controlled by `PLUGIN_AUTH_SOCKET`; the prior embedded
  baseline left it as a static plugin, producing `auth_socket.c.o` in
  `libmariadbd.a`.
- `libmylite` opens a database directory directly and already rejects account,
  role, grant, revoke, and password-management statements through
  server-surface policy coverage.

## Proposed Design

Set `PLUGIN_AUTH_SOCKET=NO` in the MyLite embedded baseline. This leaves normal
MariaDB builds unchanged while omitting the static `unix_socket` plugin from the
default embedded archive.

## Compatibility Impact

No supported `libmylite` behavior is removed. Server account management and
network or socket authentication remain outside the core embedded API. Native
storage, ordinary SQL execution, JSON, GEOMETRY, and the retained type-handler
plugins are not affected.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,478,056 bytes / 25.25 MiB with 700 members, down 2,160 bytes from the prior
26,480,216-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `PLUGIN_AUTH_SOCKET=NO` appears in the embedded CMake cache.
- Confirm `auth_socket.c.o` is absent from `libmariadbd.a`.
- Confirm the `unix_socket` plugin is absent from Information Schema plugin
  metadata.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The embedded archive omits MariaDB's static Unix socket authentication
  plugin.
- `libmylite` open/close and application SQL behavior remain unchanged.
- Server account and authentication surfaces stay explicitly outside the core
  embedded API.
