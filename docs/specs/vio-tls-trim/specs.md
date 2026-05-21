# VIO TLS Transport Trim

## Problem Statement

The default embedded profile still built MariaDB's VIO TLS transport. That
code supports TLS over client/server sockets and server acceptor state, while
MyLite's core profile is an in-process `libmylite` runtime without a network
listener or wire-protocol handshake.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/vio/viossl.c` implements TLS read/write, close, and handshake
  transport helpers.
- `mariadb/vio/viosslfactories.c` builds TLS connector and acceptor contexts.
- `mariadb/vio/vio.c` installs TLS VIO method tables for `VIO_TYPE_SSL`.
- `mariadb/vio/viosocket.c` checks TLS-buffered pending bytes.
- `mariadb/sql-common/client.c` contains inherited client-side TLS negotiation
  and certificate validation.
- `mariadb/libmysqld/libmysql.c` still exposes inherited client API symbols
  such as `mysql_ssl_set()`, but the primary MyLite API does not use network
  connections.

## Design

- Add `MYLITE_WITH_VIO_TLS`, defaulting to `ON` for upstream-style builds and
  forced `OFF` in the MyLite embedded baseline.
- When disabled, build `mylite_viossl_disabled.c` instead of `viossl.c` and
  `viosslfactories.c`.
- Compile shared VIO and client helpers without TLS transport branches when
  `MYLITE_WITH_VIO_TLS=0`.
- Fail inherited `mysql_ssl_set()` calls in the disabled profile and clear the
  inherited client SSL preference bit so TLS does not silently degrade.
- Keep OpenSSL Crypto linked because retained SQL crypto/password functions
  still use it. Do not remove SQL crypto functions in this safe trim.
- Teach the first-party imported embedded target to omit `OpenSSL::SSL` when
  the embedded MariaDB cache records `MYLITE_WITH_VIO_TLS=OFF`.

## Compatibility Impact

Network TLS transport is explicitly absent from the default embedded profile.
This does not remove SQL execution, prepared statements, native storage, JSON,
GEOMETRY/GIS, SQL crypto functions, or database-directory lifecycle behavior.

## Directory And Lifecycle Impact

No file-format change and no new database-directory companions. The trim
removes inherited network transport code that does not own durable database
state.

## Public API Impact

No `libmylite` C API change. MyLite still opens a local database directory
without a daemon, socket, or TLS handshake.

## Binary-Size And Dependency Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Backup runtime trimmed | 26,548,408 bytes / 25.32 MiB | 704 | baseline |
| VIO TLS transport trimmed | 26,536,112 bytes / 25.31 MiB | 703 | -12,296 bytes |

The pre-strip archive moved from 27,118,776 bytes to 27,106,496 bytes.

On macOS, `otool -L build/embedded-dev/packages/libmylite/mylite_embedded_open_close_test`
shows `libssl.3.dylib` is no longer linked. `libcrypto.3.dylib` remains linked
because retained SQL crypto and password functions still use OpenSSL Crypto.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg 'viossl|mylite_viossl'
otool -L build/embedded-dev/packages/libmylite/mylite_embedded_open_close_test
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_VIO_TLS=OFF` appears in the embedded CMake cache.
- `viossl.c.o` and `viosslfactories.c.o` are absent from `libmariadbd.a`.
- `mylite_viossl_disabled.c.o` is present in `libmariadbd.a`.
- First-party embedded tests link without `OpenSSL::SSL` and do not depend on
  `libssl` in the default profile.
- `libcrypto` remains available for retained SQL crypto/password functions.
- The embedded and non-embedded test suites pass.

## Risks

- Future wire-protocol adapters that need TLS must own that transport surface
  explicitly rather than inheriting it from the core embedded profile.
- Inherited client API symbols such as `mysql_ssl_set()` remain present but
  fail closed in the default embedded profile.
