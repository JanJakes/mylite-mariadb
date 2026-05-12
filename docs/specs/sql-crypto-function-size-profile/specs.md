# SQL Crypto Function Size Profile

## Problem Statement

The aggressive MyLite minsize profile still exposes MariaDB SQL functions that
exist mainly for server authentication, cryptographic helper SQL, or encrypted
server-log workflows. The current linked smoke binary still depends on
`libcrypto.so.3` even though this embedded profile has no network listener,
remote login flow, replication channel, encrypted binary log, or user account
administration model.

This slice removes the remaining OpenSSL-backed SQL crypto and password hash
function entry points from the smallest embedded profile, then measures whether
that is enough to drop linked OpenSSL code or whether auth/binlog roots still
need a deeper follow-up.

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documents these functions under encryption, hashing, and compression:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions>.
- MariaDB documents `AES_ENCRYPT()` / `AES_DECRYPT()` as AES encryption SQL
  helpers:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions/aes_encrypt>.
- MariaDB documents `RANDOM_BYTES()` as using the SSL library random generator:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions/random_bytes>.
- MariaDB documents `PASSWORD()` as a server authentication password hash
  helper, not a general application hash API:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions/password>.
- `vendor/mariadb/server/sql/item_create.cc` registers
  `AES_ENCRYPT`, `AES_DECRYPT`, `MD5`, `SHA`, `SHA1`, `SHA2`, `PASSWORD`,
  `OLD_PASSWORD`, and `RANDOM_BYTES` in `func_array`.
- `vendor/mariadb/server/sql/sql_yacc.yy` has a separate
  `PASSWORD(expr)` grammar action and an `OLD_PASSWORD('literal')` account
  syntax action that directly reference `Item_func_password`.
- `vendor/mariadb/server/sql/item_strfunc.cc` implements the retained item
  classes through `my_aes_*`, `my_md5`, `my_sha*`, and `my_random_bytes`
  wrappers.
- `vendor/mariadb/server/mysys_ssl` implements those wrappers through OpenSSL
  in `my_crypt.cc`, `my_md5.cc`, `my_sha*.cc`, and `openssl.c`.
- The current linked smoke still has undefined OpenSSL references from
  `my_crypt.cc.o`, `my_md5.cc.o`, `my_sha*.cc.o`, `openssl.c.o`, and
  `sys_vars.cc.o`. It also still retains encrypted-binlog helper symbols such
  as `Log_event_writer::encrypt_and_write()` and
  `Start_encryption_log_event`.

## Proposed Design

Add `MYLITE_DISABLE_SQL_CRYPTO_FUNCTIONS` as an off-by-default MariaDB CMake
option. The aggressive minsize script enables it.

When enabled:

- remove the OpenSSL-backed SQL crypto/password entries from `func_array`;
- make the `PASSWORD(expr)` grammar action fail explicitly instead of
  constructing `Item_func_password`;
- make account-syntax `OLD_PASSWORD('literal')` fail explicitly instead of
  calling `Item_func_password::alloc`;
- omit the corresponding item-class declarations and implementations from
  compiled code where that can be kept narrow;
- avoid linking OpenSSL crypto only if no remaining embedded objects require
  it after the SQL function roots are removed.

The first implementation should not attempt to remove the built-in
`mysql_password` authentication plugin or encrypted-binlog helpers unless the
linker evidence shows those are the remaining roots and the follow-up can be
kept separately reviewable.

## Non-Goals

- Do not remove ordinary string, date, JSON, numeric, or aggregate functions.
- Do not remove `RAND()`; only `RANDOM_BYTES()` is in scope.
- Do not change non-minsize MariaDB behavior.
- Do not claim this is a low-compatibility-cost change. These are real SQL
  functions, even if they are low value for a smallest embedded profile.
- Do not replace OpenSSL with a new crypto dependency in this slice.

## Affected Subsystems

- SQL native function registry.
- SQL parser actions for `PASSWORD()` and account password syntax.
- String-function item implementations.
- Embedded build/link profile.
- Open/close smoke coverage for disabled SQL surfaces.

## Single-File And Embedded-Lifecycle Impact

No file-format or catalog change. The slice removes SQL helper entry points
that do not fit MyLite's current local-file lifecycle and may remove a large
runtime library dependency from packaging if all OpenSSL roots are cut.

## Public API Or File-Format Impact

No public `libmylite` C API change and no `.mylite` file-format change.

## Binary-Size Impact

Before this slice, `build/mariadb-minsize-no-window` measures:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 31,138,612 |
| unstripped `mylite-open-close-smoke` | 8,151,024 |
| stripped `mylite-open-close-smoke` copy | 5,849,432 |
| vendored `libcrypto.so.3` dependency | 4,597,928 |

The direct linked SQL crypto item symbols are modest, but the package-level
prize is potentially the `libcrypto.so.3` runtime dependency. The slice must
record both linked artifact deltas and dynamic dependency changes. If
`libcrypto.so.3` remains, the spec must identify the retained roots.

After implementation, `build/mariadb-minsize-no-sql-crypto` measures:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 31,138,612 | 31,000,638 | -137,974 |
| unstripped `mylite-open-close-smoke` | 8,151,024 | 8,111,048 | -39,976 |
| stripped `mylite-open-close-smoke` copy | 5,849,432 | 5,823,416 | -26,016 |

`libcrypto.so.3` remains a dynamic dependency. The SQL function item roots were
removed, but the linked smoke still has OpenSSL references through:

- `lib_sql.cc.o`, `client.c.o`, and `password.c.o` referencing `my_sha1`;
- `sql_digest.cc.o` and `table.cc.o` referencing `my_md5`;
- `sql_acl.cc.o` referencing `my_make_scrambled_password*`;
- `log.cc.o` and `mf_iocache_encr.cc.o` referencing `my_random_bytes`;
- `encryption.cc.o` referencing `my_aes_*`.

## License, Trademark, And Dependency Impact

No new dependency or license. A successful full cut would reduce OpenSSL
runtime dependency obligations for the smallest profile. If OpenSSL remains,
there is no dependency change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-crypto \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-crypto \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sql-crypto \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Add smoke assertions that:

- `AES_ENCRYPT()` and `AES_DECRYPT()` fail explicitly or as absent functions;
- `MD5()`, `SHA()`, `SHA1()`, and `SHA2()` fail explicitly or as absent
  functions;
- `PASSWORD()` fails explicitly;
- `OLD_PASSWORD()` fails as an absent function for expression usage;
- `RANDOM_BYTES()` fails as an absent function;
- retained non-crypto utility functions such as `VERSION()` and
  `CONNECTION_ID()` still work.

Measure:

- `libmysqld/libmariadbd.a`;
- unstripped and stripped `mylite-open-close-smoke`;
- `ldd` / dynamic `NEEDED` entries;
- remaining OpenSSL undefined symbols and their archive member roots.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- Targeted SQL crypto/password functions no longer execute in the minsize
  profile.
- Retained utility functions still execute.
- Size measurements and dependency results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- `PASSWORD()` and `OLD_PASSWORD()` overlap account-management syntax as well
  as expression functions. Parser diagnostics may differ from native-registry
  missing-function diagnostics.
- The built-in authentication plugin and encrypted-binlog helpers may still
  root `mysys_ssl` and `libcrypto.so.3` after SQL functions are removed.
- Removing these functions is a high compatibility tradeoff. It should stay in
  the most aggressive size profile unless a later product decision says
  otherwise.
