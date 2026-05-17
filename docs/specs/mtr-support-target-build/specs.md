# MTR Support Target Build

## Problem

`tools/mylite-mtr-harness` prepares the MTR smoke build by invoking CMake once
for every required upstream support target. Once the build tree is current,
that prints a long sequence of `ninja: no work to do.` lines before each MTR
run and pays repeated process startup overhead without improving coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/CMakeLists.txt` defines the embedded MTR command and
  passes `--embedded-server`, `--skip-rpl`, and `MTR_BUILD_THREAD`.
- `mariadb/libmysqld/examples/CMakeLists.txt` defines
  `mariadb-test-embedded`, which the embedded MTR path expects.
- `mariadb/mysql-test/lib/My/SafeProcess/CMakeLists.txt` defines
  `my_safe_process`, which MTR uses as a process wrapper.
- CMake's build mode accepts multiple targets after `--target`, so the
  required MTR support targets can be requested from one build invocation.
- `tools/mariadb-embedded-build` already centralizes the MyLite MariaDB build
  environment, including macOS toolchain and Homebrew Bison setup.

## Design

- Extend `tools/mariadb-embedded-build build` to accept optional target
  arguments. If no target argument is supplied, keep the existing `TARGET`
  environment variable behavior and default to `libmariadbd.a`.
- Change `tools/mylite-mtr-harness` to pass its required MTR target list to one
  `tools/mariadb-embedded-build build` invocation.
- Do not change the curated MTR test list, MTR command line, pass assertion, or
  `probe` semantics.

## Compatibility Impact

No SQL, storage, C API, or compatibility-claim change. This only reduces local
MTR smoke preparation noise and process overhead.

## Single-File And Embedded-Lifecycle Impact

No `.mylite` file format, sidecar, or embedded runtime lifecycle change. The
same MTR smoke build tree remains under `build/mariadb-mtr-smoke`.

## Build, Size, And Dependencies

No new dependency and no production binary-size impact. The opt-in MTR build
still builds the same upstream test/support targets; it just requests them in
one build command.

## Test And Verification Plan

- `tools/mariadb-embedded-build build`.
- `tools/mylite-mtr-harness list`.
- `tools/mylite-mtr-harness run mylite.bootstrap_schema main.cast`.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`.
- `git diff --check`.

## Acceptance Criteria

- `tools/mariadb-embedded-build build` keeps the existing default target
  behavior.
- `tools/mylite-mtr-harness run` prepares required support targets with one
  build-wrapper invocation.
- Selected MTR smoke tests still pass.
- Docs state this as a harness build-preparation improvement, not new SQL
  compatibility coverage.

## Risks And Open Questions

- This does not solve MTR's per-test var-dir setup cost. A later slice can
  consider safe batching at the MTR execution layer, but that needs separate
  failure-isolation and pass-assertion design.
