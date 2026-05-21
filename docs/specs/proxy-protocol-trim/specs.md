# PROXY Protocol Trim

## Problem Statement

The default embedded profile still builds MariaDB's PROXY protocol listener
support even though core `libmylite` does not accept socket connections or run a
network listener. The retained parser and `proxy_protocol_networks` system
variable are server topology configuration, not SQL, native storage, or
database-directory behavior.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/proxy_protocol.cc` parses HAProxy PROXY protocol v1/v2 headers,
  parses `proxy_protocol_networks`, and checks whether a remote socket address
  may send a proxy header.
- `mariadb/sql/proxy_protocol.h` declares the parser, peer-info structure, and
  network-list lifecycle helpers.
- `mariadb/sql/net_serv.cc` compiles `handle_proxy_header()` to `IGNORE` when
  `EMBEDDED_LIBRARY` is defined, so the embedded network-read path does not
  call the parser.
- `mariadb/sql/sql_connect.cc` only calls `is_proxy_protocol_allowed()` in the
  non-embedded path guarded by `#ifndef EMBEDDED_LIBRARY`.
- `mariadb/sql/mysqld.cc` still calls `init_proxy_protocol_networks()` and
  `destroy_proxy_protocol_networks()` through shared startup and cleanup code.
- `mariadb/sql/sys_vars.cc` registers the `proxy_protocol_networks` global
  system variable and stores the backing `my_proxy_protocol_networks` pointer.

## Design

- Add `MYLITE_WITH_PROXY_PROTOCOL`, defaulting to `ON` for upstream-style builds
  and forced `OFF` in the MyLite embedded baseline.
- When disabled, build a small `mylite_proxy_protocol_disabled.cc` replacement
  object instead of `proxy_protocol.cc`.
- Keep the shared lifecycle symbols available as inert stubs so retained
  `mysqld.cc` startup and cleanup paths still link.
- Compile out the `proxy_protocol_networks` system-variable registration when
  `MYLITE_WITH_PROXY_PROTOCOL=0`, while keeping the backing global initialized
  to an empty value for retained startup code.
- Leave ordinary SQL execution, prepared statements, native storage, JSON,
  GEOMETRY/GIS, and database-directory lifecycle untouched.

## Compatibility Impact

The default embedded profile no longer exposes the PROXY protocol listener
configuration variable. This is consistent with the core library opening a local
database directory without a daemon, socket listener, or network handshake.
Wire-protocol adapters that need PROXY protocol handling must own that surface
outside the core embedded profile.

## Directory And Lifecycle Impact

No file-format change and no new database-directory companions. The trim removes
network listener configuration and parsing code that does not own durable
database state.

## Public API Impact

No `libmylite` C API change. Direct and prepared SQL that references
`@@proxy_protocol_networks` fails with MariaDB's unknown-system-variable errno
in the default embedded profile.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Replication execution sysvars trimmed | 26,534,136 bytes / 25.30 MiB | 703 | baseline |
| PROXY protocol listener trimmed | 26,527,408 bytes / 25.30 MiB | 703 | -6,728 bytes |

The pre-strip archive moved from 27,104,488 bytes to 27,097,424 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg 'proxy_protocol'
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

- `MYLITE_WITH_PROXY_PROTOCOL=OFF` appears in the embedded CMake cache.
- `proxy_protocol.cc.o` is absent from `libmariadbd.a`.
- `mylite_proxy_protocol_disabled.cc.o` is present in `libmariadbd.a`.
- `SHOW VARIABLES` does not expose `proxy_protocol_networks` in the default
  embedded profile.
- Direct and prepared `@@proxy_protocol_networks` lookups fail with MariaDB
  unknown-system-variable errno.
- JSON, GEOMETRY/GIS, native storage, transactions, and prepared statements
  remain covered by the existing embedded test suite.
- The embedded and non-embedded test suites pass.

## Risks

- Future wire-protocol adapters that need HAProxy PROXY protocol support must
  reintroduce it deliberately in their integration layer or custom profile.
- The retained MariaDB startup and cleanup paths still reference the lifecycle
  helpers, so this slice must keep fail-closed stubs rather than deleting the
  symbols outright.
