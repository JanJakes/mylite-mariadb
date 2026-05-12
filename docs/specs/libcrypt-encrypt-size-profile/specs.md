# libcrypt ENCRYPT size profile

## Problem Statement

The aggressive MyLite minsize profile still links `libcrypt.so.1` into the
linked runtime artifact. Current symbol tracing shows the only live `crypt()`
caller is MariaDB's legacy SQL `ENCRYPT()` function.

MyLite's embedded profile has no server user-management model, no remote login
flow, and no need to expose host Unix password-hash generation by default. This
slice tests removing `ENCRYPT()` from the aggressive profile so the final
linked runtime can drop the `libcrypt` dependency.

## MariaDB Base And Source References

- Base source: MariaDB Server `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Official MariaDB documentation:
  - `ENCRYPT(str[,salt])` encrypts a string using the Unix `crypt()` system
    call:
    <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions/encrypt>.
  - The same page documents that when `have_crypt=NO`, `ENCRYPT()` returns
    `NULL`.
- Current dependency evidence from `build/mariadb-minsize-vio-tls`:
  - `ldd mylite-open-close-smoke` lists `libcrypt.so.1`;
  - `/lib/aarch64-linux-gnu/libcrypt.so.1.1.0` is 198,584 bytes in the Ubuntu
    24.04 minsize container;
  - `nm -u mylite-open-close-smoke` shows a live undefined `crypt` symbol;
  - `nm -A --undefined-only libmariadbd.a` traces that symbol to
    `item_strfunc.cc.o`.
- Relevant source paths:
  - `vendor/mariadb/server/sql/item_create.cc` declares and registers
    `Create_func_encrypt`.
  - `vendor/mariadb/server/sql/item_strfunc.h` declares `Item_func_encrypt`.
  - `vendor/mariadb/server/sql/item_strfunc.cc` implements
    `Item_func_encrypt::val_str()` and calls `crypt()` under `HAVE_CRYPT`.
  - `vendor/mariadb/server/sql/sys_vars.cc` exposes `have_crypt`.
  - `vendor/mariadb/server/libmysqld/CMakeLists.txt` links `${LIBCRYPT}` into
    the embedded archive target's final link set.

## Scope

Add a MyLite-only aggressive minsize option:

```text
MYLITE_DISABLE_CRYPT_FUNCTION=ON
```

When enabled, the profile will:

- omit the `ENCRYPT()` native-function builder from `item_create.cc`;
- omit the `Item_func_encrypt` declaration and method body;
- avoid linking `${LIBCRYPT}` into `libmysqld` when no other retained embedded
  code needs it;
- keep `HAVE_CRYPT` and `have_crypt` detection unchanged for non-MyLite builds.

`ENCRYPT()` should fail through MariaDB's unknown-function path in this profile,
matching other removed optional SQL functions.

## Non-Goals

- Do not remove `AES_ENCRYPT()`, `AES_DECRYPT()`, `DES_ENCRYPT()`,
  `DES_DECRYPT()`, `ENCODE()`, `DECODE()`, `MD5()`, `SHA*()`,
  `RANDOM_BYTES()`, `PASSWORD()`, or `OLD_PASSWORD()` in this slice.
- Do not remove OpenSSL `libcrypto.so.3`.
- Do not change parser grammar, stored metadata, MyLite file format, or public
  `libmylite` API.
- Do not claim `have_crypt=NO` unless the build actually changes that system
  variable's source value. The expected user-visible result is unknown
  function, not `NULL`.

## Proposed Design

1. Define `MYLITE_DISABLE_CRYPT_FUNCTION` in MariaDB's embedded minsize build
   configuration and enable it from `tools/build-mariadb-minsize.sh`.
2. Guard the `Create_func_encrypt` class, singleton, factory method, and
   `func_array` registration in `item_create.cc`.
3. Guard `Item_func_encrypt` in `item_strfunc.h` and its `val_str()` method in
   `item_strfunc.cc`.
4. Link `${LIBCRYPT}` into `libmysqld` only when the crypt function is retained.
5. Extend the open/close smoke to assert `ENCRYPT()` now fails as an unknown
   function while retained crypto and ordinary string functions still work.
6. Update production-size analysis with archive, linked runtime, stripped
   runtime, and dynamic dependency deltas.

## Affected MariaDB Subsystems

- Embedded minsize build profile.
- Native SQL function registry.
- String-function item classes.
- Embedded final link dependency set.
- Minsize compatibility smoke coverage.

## DDL Metadata Routing Impact

None. This slice removes one scalar SQL function and does not touch DDL,
catalog persistence, table discovery, or storage-engine routing.

## Single-File And Embedded-Lifecycle Impact

No intended impact. The removed function does not affect database open/close,
sidecar policy, recovery, locks, or MyLite-owned file layout.

## Public API Or File-Format Impact

No public C API or `.mylite` file-format change.

SQL compatibility impact: the aggressive minsize profile no longer supports
MariaDB's legacy `ENCRYPT()` SQL function. This is an explicit size/profile
tradeoff for the embedded default.

## Binary-Size And Dependency Impact

Expected savings are modest in the linked binary and static archive, but the
dependency effect is useful for bundled distributions:

- remove one `crypt` PLT/import from linked runtime artifacts;
- remove `libcrypt.so.1` from `ldd` if no other retained object roots it;
- avoid about 198,584 bytes in a Linux ARM64 bundle that vendors runtime
  libraries.

## License, Trademark, And Dependency Impact

No new dependencies. This reduces the runtime dependency set in the aggressive
profile. MariaDB-derived code remains GPL-2.0-only under the existing project
license.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-libcrypt \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-libcrypt \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-libcrypt \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Inspect:

```sh
ldd build/mariadb-minsize-libcrypt/mylite/mylite-open-close-smoke |
  grep -E 'libcrypt|libcrypto|libssl' || true
nm -u build/mariadb-minsize-libcrypt/mylite/mylite-open-close-smoke |
  grep -E '(^| )crypt$' || true
nm -C build/mariadb-minsize-libcrypt/mylite/mylite-open-close-smoke |
  grep -E 'Item_func_encrypt|Create_func_encrypt' || true
```

Expected inspection result:

- no `libcrypt.so.1` in `ldd`;
- no undefined `crypt` symbol;
- no `Item_func_encrypt` or `Create_func_encrypt` symbols in the linked smoke.

## Acceptance Criteria

- `MYLITE_DISABLE_CRYPT_FUNCTION=ON` is enabled by the aggressive minsize
  build script.
- `ENCRYPT()` fails through the unknown-function path in the open/close smoke.
- Retained crypto functions used by current smokes, especially
  `RANDOM_BYTES()`, still execute.
- Build, open/close smoke, and compatibility harness pass.
- `libcrypt.so.1` is absent from the linked smoke runtime dependencies.
- Production-size analysis records size and dependency deltas.

## Risks And Unresolved Questions

- If another inherited object roots `crypt()` after this guard, `${LIBCRYPT}`
  cannot be removed in the same narrow slice.
- `have_crypt` may still report `YES` because platform detection remains
  unchanged. That is acceptable for this size attempt if `ENCRYPT()` itself is
  no longer registered, but it should be documented in the result.
- This is a compatibility tradeoff. Applications that rely on MariaDB's legacy
  `ENCRYPT()` function would need a non-minsize profile or a compatibility
  decision to retain `libcrypt`.
