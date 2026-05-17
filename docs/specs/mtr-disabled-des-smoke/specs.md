# MTR Disabled DES Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.func_encrypt_nossl`,
covering MariaDB's deprecated DES encryption functions when DES support is
disabled in the embedded smoke profile.

## Non-Goals

- Full encryption-function MTR coverage.
- Enabling SSL, OpenSSL DES, or key-file management surfaces.
- Normalizing upstream MTR expected-result files for storage-engine drift.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_encrypt_nossl.test` is an embedded-only scalar
  function test guarded by `include/is_embedded.inc`.
- The test exercises `DES_ENCRYPT()` and `DES_DECRYPT()` calls with string,
  numeric, default-key, `NULL`, wrong-password, and `FLUSH DES_KEY_FILE`
  paths.
- `mariadb/mysql-test/main/func_encrypt_nossl.result` records deterministic
  `NULL` results plus MariaDB deprecation and disabled-feature warnings when
  DES support is not available.
- The test has no durable table DDL, native storage-engine dependency,
  sequence dependency, host-file import/export dependency, or daemon-only
  prelude.
- Verified command:
  `tools/mylite-mtr-harness run main.func_encrypt_nossl`.

## Compatibility Impact

The compatibility matrix and roadmap can say the opt-in embedded MTR smoke
runner covers selected disabled DES encryption-function behavior. This remains
curated opt-in MTR smoke coverage, not full crypto, SSL, or MTR-scale SQL
coverage.

## Design

- Add `main.func_encrypt_nossl` to `tools/mylite-mtr-harness`'s default curated
  list.
- Leave MariaDB test files unchanged.

## File Lifecycle

No `.mylite` file, companion-file, or storage lifecycle behavior changes. This
is test-harness coverage only.

## Embedded Lifecycle And API

No public C API or embedded startup behavior changes.

## Build, Size, And Dependencies

No production dependency or size-profile change. The existing MTR smoke build
continues to build the same upstream test support targets.

## Test Plan

- `tools/mylite-mtr-harness list`.
- `tools/mylite-mtr-harness run main.func_encrypt_nossl`.
- `tools/mylite-mtr-harness run`.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`.
- `find mariadb/mysql-test -name '*.reject' -print`.
- `git diff --check`.

## Acceptance Criteria

- The default MTR smoke list includes `main.func_encrypt_nossl`.
- `main.func_encrypt_nossl` passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This test proves MariaDB's disabled-DES behavior in the current embedded
  profile; it does not prove enabled DES, SSL, or external key-file support.
- Broader encryption-function suites may hit disabled optional dependencies or
  server-oriented surfaces and need separate source-backed evaluation.
