# VIO TLS Size Profile

## Problem

The current aggressive minsize profile still links `libssl.so.3` into the
linked MyLite runtime artifact. MyLite's embedded profile has no network
listener, remote client login flow, or replication channel, so MariaDB's VIO
TLS transport code is not useful in the default embedded environment.

Earlier `no-ssl-build-profile` work proved that full `WITH_SSL=OFF` is too
broad: SQL and authentication crypto helpers still need OpenSSL-backed digest,
AES, DES, random, and KDF primitives. This slice splits TLS transport removal
from crypto removal.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `vendor/mariadb/server/vio/CMakeLists.txt` always builds `viossl.c` and
  `viosslfactories.c`, and links VIO with `${SSL_LIBRARIES}`.
- `vendor/mariadb/server/vio/vio.c` installs SSL read/write/close callbacks
  for `VIO_TYPE_SSL` under `HAVE_OPENSSL`.
- `vendor/mariadb/server/vio/viosocket.c` calls `SSL_pending()` in
  `vio_is_connected()` and `vio_pending()`.
- `vendor/mariadb/server/sql-common/client.c` still contains client-side TLS
  setup, TLS connector cleanup, certificate validation, and self-signed
  certificate checks even when built into `libmysqld`.
- `vendor/mariadb/server/mysys_ssl/CMakeLists.txt` links `${SSL_LIBRARIES}`
  even though the retained digest and cipher helpers need `libcrypto`, not
  VIO's TLS transport layer.
- `vendor/mariadb/server/libmysqld/CMakeLists.txt` links the embedded archive
  against `${SSL_LIBRARIES}`.

Current dependency evidence from `build/mariadb-minsize-icf`:

- linked `mylite-open-close-smoke` depends on both `libssl.so.3` and
  `libcrypto.so.3`;
- `/usr/lib/aarch64-linux-gnu/libssl.so.3` is 737,192 bytes in the Ubuntu
  24.04 minsize container;
- `/usr/lib/aarch64-linux-gnu/libcrypto.so.3` is 4,597,928 bytes and is not in
  scope for this slice.

## Design

Add a MyLite-only minsize option named `MYLITE_DISABLE_VIO_SSL`.

When enabled:

- compile out VIO's TLS source files (`viossl.c`, `viosslfactories.c`);
- compile out VIO's `VIO_TYPE_SSL` callback setup and `SSL_pending()` paths;
- compile out embedded/client TLS connector setup and certificate validation
  paths in `sql-common/client.c`;
- keep OpenSSL headers and `HAVE_OPENSSL` for retained SQL/auth crypto helpers;
- link `mysys_ssl` and `libmysqld` with `OPENSSL_CRYPTO_LIBRARY` rather than
  full `${SSL_LIBRARIES}` when the system OpenSSL split library is available;
- leave `libcrypto.so` linked until a separate SQL/auth crypto slice removes or
  replaces those users.

Update `tools/build-mariadb-minsize.sh` to enable
`MYLITE_DISABLE_VIO_SSL=ON` for the aggressive minsize profile.

## Non-Goals

- Do not set `WITH_SSL=OFF`.
- Do not remove SQL crypto functions such as `MD5()`, `SHA*()`,
  `AES_ENCRYPT()`, `RANDOM_BYTES()`, or password-auth helper code.
- Do not remove OpenSSL headers or libcrypto.
- Do not change public `libmylite` API or `.mylite` file format.
- Do not remove SQL syntax for `REQUIRE SSL` or replication `MASTER_SSL*`
  clauses in this slice.

## Affected Subsystems

- VIO transport library.
- Embedded client/common connection code.
- Minsize build/link profile.
- Production-size dependency analysis.

## DDL Metadata Routing Impact

None. This slice does not touch table definitions, catalog persistence, or
storage-engine DDL routing.

## Single-File and Embedded-Lifecycle Impact

No intended impact. MyLite does not expose network/TLS transport in the default
embedded API. Open/close lifecycle, durable file layout, sidecar policy,
locking, and recovery should remain unchanged.

## Public API and File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

Compatibility impact is limited to inherited MariaDB C API behavior inside the
aggressive embedded profile: TLS client/server transport is not available.
That fits the documented unsupported server surface for MyLite's default
embedded runtime.

## Binary-Size Impact

Expected effects:

- remove `libssl.so.3` from linked smoke dependencies;
- reduce linked runtime size by the VIO TLS object code and relocations that
  remain live;
- keep `libcrypto.so.3` as a dynamic dependency;
- keep the static archive change modest because most dynamic-library savings
  are outside `libmariadbd.a`.

## License, Trademark, and Dependency Impact

No new dependency. This reduces the dynamic dependency set for the aggressive
profile by removing `libssl.so.3` when the platform OpenSSL package splits TLS
and crypto libraries.

## Test Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Measure:

```sh
ldd build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke
stat -c "%s" build/mariadb-minsize-vio-tls/libmysqld/libmariadbd.a
cp build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke \
  build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke.stripped
strip --strip-unneeded \
  build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke.stripped
stat -c "%s" \
  build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke.stripped
nm -u build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke |
  grep -E 'SSL_|OPENSSL_init_ssl|X509_|sslconnect|sslaccept' || true
```

## Acceptance Criteria

- The minsize build profile enables `MYLITE_DISABLE_VIO_SSL=ON`.
- `libssl.so.3` is absent from `ldd` output for `mylite-open-close-smoke`.
- `libcrypto.so.3` may remain present.
- Build, open/close smoke, and compatibility harness pass.
- Linked runtime and dependency deltas are recorded in
  `docs/research/production-size-analysis.md`.

## Implementation Results

The implementation adds `MYLITE_DISABLE_VIO_SSL=ON` to the aggressive minsize
profile, omits `viossl.c` and `viosslfactories.c`, links VIO without
`${SSL_LIBRARIES}`, and links retained crypto helpers against
`OPENSSL_CRYPTO_LIBRARY` where available.

Final measurements from `build/mariadb-minsize-vio-tls`:

| Artifact | Bytes | Delta from ICF profile |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 32,261,482 | -21,898 |
| `vio/libvio.a` | 26,706 | n/a |
| `mylite/mylite-open-close-smoke` | 8,479,248 | -15,112 |
| stripped `mylite-open-close-smoke` copy | 6,083,040 | -11,528 |

Runtime dependency evidence:

- `ldd build/mariadb-minsize-vio-tls/mylite/mylite-open-close-smoke` no
  longer lists `libssl.so.3`;
- `libcrypto.so.3` remains listed;
- the only OpenSSL-family undefined symbol matched by the SSL scan is
  `OpenSSL_version`, which is provided by `libcrypto`.

If a Linux package vendors OpenSSL libraries, this also avoids the
737,192-byte Ubuntu 24.04 ARM64 `libssl.so.3` dependency. It does not avoid
the 4,597,928-byte `libcrypto.so.3` dependency.

Verified with:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-vio-tls \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

## Risks and Unresolved Questions

- MariaDB's client C API code is shared between embedded and client builds;
  guards must stay narrowly tied to the MyLite minsize option.
- Some retained code may still compare against `VIO_TYPE_SSL`; that is fine if
  no TLS VIO can be constructed in this profile.
- Removing `libssl.so` does not solve the larger 4.39 MiB `libcrypto.so`
  dependency. That needs a separate SQL/auth crypto decision.
