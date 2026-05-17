# MTR KDF Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.func_kdf`, covering
MariaDB's scalar `KDF()` and AES integration behavior under the embedded smoke
profile.

## Non-Goals

- Full crypto-function MTR coverage.
- Enabling SSL-only or server-only crypto surfaces.
- Normalizing upstream MTR expected-result files for default-engine drift.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/func_kdf.test` is a scalar-function test with no
  table DDL, native storage-engine dependency, sequence dependency, or host
  file I/O.
- `mariadb/mysql-test/main/func_kdf.result` records deterministic HKDF,
  PBKDF2-HMAC, warning, length-boundary, and AES encrypt/decrypt results.
- Under the MyLite MTR smoke profile, MTR reports this test as
  `main.func_kdf 'new' [ pass ]`. The quoted variant is a passing result for
  the selected exact case, so the harness pass assertion must allow an optional
  quoted variant between the case name and `[ pass ]`.

## Compatibility Impact

The compatibility matrix and roadmap can say the opt-in embedded MTR smoke
runner covers selected crypto/KDF scalar behavior. This remains opt-in curated
smoke coverage, not full MTR-scale SQL compatibility.

## Design

- Add `main.func_kdf` to `tools/mylite-mtr-harness`'s default curated list.
- Teach `assert_test_passed()` to accept MTR pass lines with an optional quoted
  variant suffix after the selected `suite.case` name.
- Leave MariaDB test files unchanged.

## File Lifecycle

No `.mylite` file, companion-file, or storage lifecycle behavior changes. This
is test-harness coverage only.

## Embedded Lifecycle And API

No public C API or embedded startup behavior changes.

## Build, Size, And Dependencies

No production dependency or size-profile change. The existing MTR smoke build
continues to build the same test support targets.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.func_kdf`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.func_kdf`.
- `main.func_kdf` passes under the MyLite MTR smoke profile despite the quoted
  MTR variant suffix.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- The harness still intentionally requires a pass line for each selected test;
  it does not accept skipped tests or unselected variants.
- Broader crypto-function suites may hit disabled SSL, zlib, native-engine, or
  expected-result normalization gaps and need separate evaluation.
