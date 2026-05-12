# Server Encryption Size Profile

## Problem Statement

After the SQL crypto function cut, the aggressive MyLite minsize profile still
links OpenSSL through inherited server-side encryption paths. These paths serve
MariaDB daemon features such as encrypted binary logs, relay logs, and
encrypted temporary IO cache files. They do not fit the current embedded
single-file profile, which has no replication channel, no durable binlog
surface, and no key-management plugin model.

This slice removes server-side encryption roots from the minsize embedded
profile so the next OpenSSL investigation can focus on the remaining password,
client-auth, SQL digest, and view-MD5 roots.

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documents binary log encryption as a server data-at-rest feature
  controlled by `encrypt_binlog` and backed by a key-management/encryption
  plugin:
  <https://mariadb.com/docs/server/security/encryption/data-at-rest-encryption/encrypting-binary-logs>.
- `vendor/mariadb/server/sql/encryption.cc` initializes MariaDB's encryption
  service and falls back to AES helpers from `mysys_ssl/my_crypt.cc`.
- `vendor/mariadb/server/sql/mf_iocache_encr.cc` enables encrypted temporary
  IO cache files when `encrypt_tmp_files` is on and uses `my_random_bytes()`
  plus the encryption service.
- `vendor/mariadb/server/sql/log.cc` writes `START_ENCRYPTION_EVENT` and
  calls `my_random_bytes()` when binary log encryption is enabled.
- The current `build/mariadb-minsize-no-sql-crypto` linked smoke still has
  OpenSSL references from `my_crypt.cc.o` because `encryption.cc.o`,
  `mf_iocache_encr.cc.o`, and `log.cc.o` reference `my_aes_*` and
  `my_random_bytes`.

## Proposed Design

Add `MYLITE_DISABLE_SERVER_ENCRYPTION` as an off-by-default MariaDB CMake
option. The aggressive minsize script enables it.

When enabled:

- omit `sql/encryption.cc` and `sql/mf_iocache_encr.cc` from the embedded SQL
  archive;
- link a small MyLite minsize stub that exposes the retained encryption-service
  symbols with disabled/no-key behavior;
- make `init_io_cache_encryption()` a no-op that leaves inherited encrypted
  temporary-file hooks inactive;
- make `encrypt_binlog`, `encrypt_tmp_files`, and
  `encrypt_tmp_disk_tables` read-only and unavailable as command-line options
  in the minsize profile;
- guard the binary-log encryption branch in `log.cc` so it rejects attempted
  use without referencing `my_random_bytes()`;
- keep non-minsize MariaDB behavior unchanged.

## Non-Goals

- Do not remove SQL digest MD5, view MD5, `mysql_native_password`, embedded
  client password scrambling, or server UID SHA1 roots in this slice.
- Do not add a replacement crypto dependency.
- Do not claim `libcrypto.so.3` is gone unless the measured linked artifact
  actually drops the dependency.
- Do not change MyLite file-format behavior.

## Affected Subsystems

- Embedded source selection.
- MariaDB encryption service symbols.
- Binary-log creation path.
- Temporary IO-cache encryption initialization.
- Build/link profile and dependency evidence.

## Single-File And Embedded-Lifecycle Impact

The slice removes inherited daemon encryption hooks that would require external
key-management plugin state and server-owned log/temp files. MyLite still needs
a first-party design for any future `.mylite` file encryption, but this slice
does not introduce one.

## Public API Or File-Format Impact

No public `libmylite` C API change and no `.mylite` file-format change.

## Binary-Size Impact

Before this slice, `build/mariadb-minsize-no-sql-crypto` measures:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 31,000,638 |
| unstripped `mylite-open-close-smoke` | 8,111,048 |
| stripped `mylite-open-close-smoke` copy | 5,823,416 |

Expected direct savings are the omitted `encryption.cc.o` and
`mf_iocache_encr.cc.o` bodies plus any linked AES/random helper sections that
become unreachable. `libcrypto.so.3` may still remain because password/auth and
MD5/SHA1 roots are still present.

After this slice, `build/mariadb-minsize-no-server-encryption` measures:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,983,136 | -17,502 |
| unstripped `mylite-open-close-smoke` | 8,099,344 | -11,704 |
| stripped `mylite-open-close-smoke` copy | 5,815,856 | -7,560 |

The linked smoke no longer defines `my_aes_*` or `my_random_bytes`.
`libcrypto.so.3` remains because retained objects still reference `my_sha1`
from `lib_sql.cc.o`, `client.c.o`, and `password.c.o`, `my_md5` from
`sql_digest.cc.o` and `table.cc.o`, `my_make_scrambled_password*` from
`sql_acl.cc.o`, and OpenSSL startup compatibility helpers from `openssl.c.o`.

## License, Trademark, And Dependency Impact

No new dependency or license. If this removes all AES/random OpenSSL roots but
not `libcrypto.so.3`, the dependency status remains unchanged until the
remaining digest/auth roots are removed or replaced.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- `libmysqld/libmariadbd.a`;
- unstripped and stripped `mylite-open-close-smoke`;
- dynamic `NEEDED` entries;
- remaining OpenSSL undefined symbols and their archive member roots.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- The linked artifact no longer has `my_aes_*` or `my_random_bytes` roots from
  server encryption paths.
- Any remaining `libcrypto.so.3` dependency is traced to non-server-encryption
  roots and recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-server-encryption \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

The open/close smoke report recorded
`exec_server_encryption_rows=0:0:0` and verified SQL `SET GLOBAL` attempts for
`encrypt_binlog`, `encrypt_tmp_files`, and `encrypt_tmp_disk_tables` fail as
read-only variables.

## Risks And Unresolved Questions

- Some retained binlog code still references the encryption service structure.
  The stub must preserve those symbols without pulling OpenSSL.
- `encrypt_binlog=ON` and encrypted temporary-file options should remain
  ineffective in the aggressive embedded profile. If a user can still request
  them, the behavior must fail closed rather than silently writing encrypted
  sidecars.
- Removing the last `my_crypt.cc.o` root will not remove OpenSSL by itself if
  MD5/SHA1 password and digest roots remain.
