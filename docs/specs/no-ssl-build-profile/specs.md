# No-SSL embedded build profile

## Problem

The current MyLite minimal embedded build still configures MariaDB with
`WITH_SSL=system`. That links OpenSSL into the `mylite-open-close-smoke`
proxy even though MyLite's current embedded profile has no network listener,
no client authentication flow, and no replication channel. If a production
package vendors runtime libraries, OpenSSL accounts for about 5.09 MiB of
external dependency files before compression.

This slice tests and, if viable, makes MyLite's minimal embedded profile build
without SSL support.

## Source findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `tools/build-mariadb-minsize.sh` currently passes `-DWITH_SSL=system`.
- `vendor/mariadb/server/cmake/ssl.cmake` initializes `WITH_SSL` to `yes`
  and accepts only `yes`, `system`, `bundled`, or an existing custom OpenSSL
  path. Passing `WITH_SSL=no` or `WITH_SSL=OFF` currently fails at configure
  time.
- `vendor/mariadb/server/sql/CMakeLists.txt` adds `${SSL_DEFINES}` to SQL
  compilation and links `${SSL_LIBRARIES}` into the server SQL target.
- `vendor/mariadb/server/libmysqld/CMakeLists.txt` also adds
  `${SSL_DEFINES}` and `${SSL_INCLUDE_DIRS}` to embedded compilation.
- MariaDB SQL sources already guard SSL-dependent behavior with
  `HAVE_OPENSSL`, including `mysqld.cc`, `sql_acl.cc`, `sql_connect.cc`,
  `sql_parse.cc`, `sql_repl.cc`, and `sys_vars.cc`.

## Design

Add a MyLite-only build profile path that can set `WITH_SSL=OFF` and leave
`SSL_DEFINES`, `SSL_LIBRARIES`, `SSL_INCLUDE_DIRS`, and
`SSL_INTERNAL_INCLUDE_DIRS` empty.

Update `tools/build-mariadb-minsize.sh` to use `-DWITH_SSL=OFF` for the
current minimal embedded profile.

This does not remove all cryptographic functionality. It removes OpenSSL-backed
server SSL/TLS support from the embedded build. Password hashing and other
non-OpenSSL code that MariaDB builds without SSL remain available if their
sources compile in the no-SSL configuration.

## Non-goals

- Do not add a public runtime option for SSL.
- Do not implement network behavior.
- Do not remove SQL syntax or system variables beyond what MariaDB already
  compiles out when `HAVE_OPENSSL` is absent.
- Do not claim this is the final production packaging shape for all platforms.

## Compatibility impact

This is acceptable for the current MyLite embedded profile because MyLite does
not expose a daemon, socket listener, replication channel, remote client login,
or TLS configuration API.

The compatibility cost is that SSL/TLS-specific MariaDB server behavior is not
available in the minimal embedded build. That is aligned with MyLite's existing
unsupported-server-surface policy.

## Single-file and embedded-lifecycle impact

The slice should not change database file format, catalog layout, durable
sidecar policy, or open/close lifetime semantics. The expected effect is only
build profile and dependency footprint.

## Binary-size impact

Expected effects:

- remove `libssl.so` and `libcrypto.so` from linked smoke dependencies if no
  remaining target pulls them in,
- reduce linked binary size where OpenSSL-dependent code and relocations are
  excluded,
- possibly reduce `libmariadbd.a` if SSL-guarded code compiles out.

The implementation must record the new archive size, linked proxy size,
stripped proxy size, and dynamic dependencies.

## Test plan

Run:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Also inspect:

```sh
ldd build/mariadb-minsize/mylite/mylite-open-close-smoke
```

The linked smoke should not depend on `libssl.so` or `libcrypto.so`.

## Acceptance criteria

- The minimal build configures successfully with SSL disabled.
- `libmariadbd.a` and `mylite-open-close-smoke` build.
- Current MyLite open/close and compatibility harness smokes pass.
- No unexpected durable MyLite sidecars appear.
- Production-size analysis is updated with measured size deltas.

## Risks

- Some inherited MariaDB startup path may assume SSL exists despite
  `HAVE_OPENSSL` guards.
- Some authentication, replication, or system variable behavior may change.
  That is acceptable only if it remains within documented unsupported server
  surfaces for MyLite's embedded profile.
- CMake changes in upstream-derived files must stay narrow to keep future
  rebases reviewable.
