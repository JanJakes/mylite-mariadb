# MTR Window Engine Normalization

## Problem Statement

`main.win_first_last_value` and `main.win_percentile` exercise useful upstream
window-function behavior, but they fail in the MyLite MTR smoke profile only
because `SHOW CREATE TABLE` prints the configured Aria default engine:

- `ENGINE=Aria ... PAGE_CHECKSUM=1`

The upstream result files expect the historical MyISAM default-engine text:

- `ENGINE=MyISAM ...`

This mismatch is not a SQL behavior failure. It is the same default-engine
text difference already normalized in other accepted MTR smoke tests.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/win_first_last_value.test` covers
  `FIRST_VALUE()` and `LAST_VALUE()` over ordered rows, explicit window
  frames, partitioned mixed-type rows, and CTAS result-type preservation.
- `mariadb/mysql-test/main/win_percentile.test` covers
  `PERCENTILE_DISC()`, `PERCENTILE_CONT()`, and `MEDIAN()` syntax, runtime
  results, error handling, view use, CTAS result-type preservation, unsigned
  integer preservation, decimal preservation, and temporal/string inputs.
- The MyLite MTR smoke profile runs with
  `--mysqld=--default-storage-engine=Aria` and
  `MYLITE_MTR_DEFAULT_STORAGE_ENGINE=Aria` because native MyISAM is omitted
  from the trimmed embedded build.
- Existing accepted tests such as `main.cast`, `main.case`, `main.type_int`,
  `main.type_varchar`, `main.create-uca`, `main.constraints`, and related
  charset include files already use:

  ```text
  --replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""
  ```

  immediately before affected `SHOW CREATE TABLE` statements.
- A focused probe of these two window tests failed only on the Aria/MyISAM
  `SHOW CREATE TABLE` output mismatch.

## Scope

- Normalize the known default-engine text mismatch in the two upstream-derived
  window tests using MariaDB MTR's existing `--replace_result` directive.
- Add both tests to the default curated MTR smoke list.
- Update compatibility and roadmap docs to name the newly admitted
  first/last-value and percentile/median coverage.

## Non-Goals

- Add generic post-processing to the harness output.
- Normalize arbitrary result differences.
- Enable native MyISAM or other omitted engines.
- Treat these MTR cases as routed MyLite storage evidence.

## Design

Add `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""`
immediately before the affected `SHOW CREATE TABLE t2` statements. This keeps
the upstream expected result files intact and uses the same MTR-local mechanism
already present in accepted tests.

The harness continues to require a real MTR `[ pass ]` line. No custom
post-run diff interpretation is added.

## File Lifecycle

No `.mylite` file-format or runtime lifecycle change. MTR artifacts remain
under `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API or embedded startup change.

## Storage-Engine Routing Impact

No MyLite routed-storage behavior changes. The normalization documents the
MTR smoke profile's Aria default-engine text, not final MyLite storage routing.

## Build, Size, And Dependencies

No production build, binary-size, or dependency change.

## Test Plan

- `tools/mylite-mtr-harness probe main.win_first_last_value main.win_percentile`
- `tools/mylite-mtr-harness run main.win_first_last_value main.win_percentile`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Verification Evidence

- `tools/mylite-mtr-harness probe main.win_first_last_value main.win_percentile`
  - both tests passed.
- `tools/mylite-mtr-harness run main.win_first_last_value main.win_percentile`
  - both tests reported MTR `[ pass ]`.
- `tools/mylite-mtr-harness run`
  - `mylite.bootstrap_schema` passed.
  - all 150 selected `main` tests passed.
- `tools/mylite-mtr-harness list | wc -l`
  - `151`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
  - no reject files.
- `git diff --check`

## Acceptance Criteria

- Both normalized window tests report MTR `[ pass ]` under the MyLite smoke
  profile.
- The full curated MTR smoke list passes.
- The harness still admits only tests that MTR reports as passed.
- No result files are regenerated or edited.

## Risks And Open Questions

- This pattern should remain scoped to known default-engine text mismatches.
  A broader normalization facility would need separate policy because it could
  hide real SQL, metadata, or optimizer differences.
- These tests run against the embedded MariaDB baseline profile, not MyLite's
  routed durable storage path.
