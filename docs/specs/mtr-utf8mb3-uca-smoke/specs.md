# MTR utf8mb3 UCA Smoke

## Problem Statement

The curated MTR smoke runner already covers selected ASCII, legacy, UTF-32,
Latin2, utf8mb3 binary/general, utf8mb4 binary/general, and utf8mb4 UCA 1400
charset behavior. It does not yet include passing upstream coverage for
utf8mb3 UCA 1400 and utf8mb3 general-1400 edge cases that run cleanly under
the trimmed embedded profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ctype_utf8mb3_uca1400_ai_ci.test` sources the
  shared special-character, `WEIGHT_STRING()`, regexp, and LIKE condition
  propagation charset includes under `utf8mb3_uca1400_ai_ci`.
- `mariadb/mysql-test/main/ctype_utf8mb3_geeral1400_as_ci.test` covers the
  upstream MDEV-33806 regression shape for
  `utf8mb3_general1400_as_ci`, including indexed `LIKE` reads before and
  after inserts. The upstream filename contains `geeral`.
- Both tests pass under the MyLite MTR smoke profile without upstream
  test-file or result-file changes.
- Rejected nearby probes hit known trimmed-profile boundaries: native MyISAM
  requirements, Sequence-engine skips, disabled dynamic-column runtime,
  case-sensitive filesystem requirements, old MyISAM file fixtures, and
  result differences that need separate policy.

## Scope

- Add `main.ctype_utf8mb3_uca1400_ai_ci` and
  `main.ctype_utf8mb3_geeral1400_as_ci` to the default curated MTR smoke list.
- Update compatibility and roadmap docs to name the utf8mb3 UCA/general-1400
  coverage.

## Non-Goals

- Normalize additional charset result differences.
- Enable omitted native MyISAM, Sequence, dynamic-column, or file-I/O surfaces.
- Add exhaustive charset coverage.
- Treat MTR charset behavior as routed MyLite storage evidence.

## Design

The harness change is list-only. Both selected tests already produce real MTR
`[ pass ]` lines under the current smoke profile, so no upstream test changes
or expected result edits are required.

## File Lifecycle

No `.mylite` file-format or runtime lifecycle change. MTR artifacts remain
under `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API or embedded startup change.

## Build, Size, And Dependencies

No production build, binary-size, or dependency change.

## Test Plan

- `tools/mylite-mtr-harness probe main.ctype_utf8mb3_uca1400_ai_ci main.ctype_utf8mb3_geeral1400_as_ci`
- `tools/mylite-mtr-harness run main.ctype_utf8mb3_uca1400_ai_ci main.ctype_utf8mb3_geeral1400_as_ci`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Verification Evidence

- `tools/mylite-mtr-harness probe main.ctype_utf8mb3_uca1400_ai_ci main.ctype_utf8mb3_geeral1400_as_ci`
  - both tests passed.
- `tools/mylite-mtr-harness run main.ctype_utf8mb3_uca1400_ai_ci main.ctype_utf8mb3_geeral1400_as_ci`
  - both tests reported MTR `[ pass ]`.
- `tools/mylite-mtr-harness run`
  - `mylite.bootstrap_schema` passed.
  - all 151 selected `main` tests passed.
- `tools/mylite-mtr-harness list | wc -l`
  - `153`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
  - no reject files.
- `git diff --check`

## Acceptance Criteria

- Both utf8mb3 charset tests report MTR `[ pass ]` under the MyLite smoke
  profile.
- The full curated MTR smoke list passes.
- No upstream MariaDB test files or result files are modified.

## Risks And Open Questions

- Nearby charset tests remain outside the default list until their native
  engine, omitted surface, environment-specific, or result-normalization
  blockers are handled explicitly.
- This remains upstream MariaDB embedded baseline coverage, not routed MyLite
  durable-storage coverage.
