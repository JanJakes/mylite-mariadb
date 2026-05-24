# MTR Funcs Information Schema Smoke

## Goal

Promote passing upstream MTR rows that cover retained `funcs_1`
information-schema metadata and one optimizer regression under the embedded
smoke profile:

- `funcs_1.is_character_sets`
- `funcs_1.is_coll_char_set_appl`
- `funcs_1.is_collations`
- `funcs_1.is_engines`
- `funcs_1.is_engines_memory`
- `funcs_1.is_schemata_embedded`
- `funcs_1.is_statistics_is`
- `funcs_1.is_table_constraints_is`
- `optimizer_unfixed_bugs.bug49129`

## Non-Goals

- Broad `funcs_1`, `funcs_2`, or optimizer-unfixed-suite coverage.
- Re-enabling native MyISAM/InnoDB, server accounts, privilege-filtered
  protocol sessions, host-file SQL I/O, or profile-specific result
  normalization.
- Running these rows against MyLite storage-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- The selected `funcs_1.is_*` rows query retained
  `INFORMATION_SCHEMA.CHARACTER_SETS`, `CHARACTER_SET_APPLICABILITY`,
  `COLLATIONS`, `ENGINES`, `SCHEMATA`, `STATISTICS`, and
  `TABLE_CONSTRAINTS` metadata without requiring server accounts or
  not-embedded protocol sessions.
- `funcs_1.is_engines_memory` covers retained MEMORY engine metadata. This is
  compatible with MyLite's target to keep zero-file MEMORY/HEAP behavior
  explicit.
- `optimizer_unfixed_bugs.bug49129` passes as an ordinary optimizer regression
  under the embedded smoke profile.
- Probed candidates intentionally left out of accepted coverage:
  - `funcs_1.row_count_func` fails on `SELECT ... INTO OUTFILE`, which is
    disabled in the embedded profile.
  - `funcs_1.is_tables_embedded` and `funcs_1.is_tables_is_embedded` fail on
    profile-specific table metadata/result-shape differences.
  - `funcs_2.memory_charset` fails on `LOAD DATA INFILE`, which is disabled in
    the embedded profile.
  - `funcs_2.myisam_charset` fails on native `ENGINE=MyISAM` in the baseline
    profile and remains a future routed-engine compatibility candidate.

## Compatibility Impact

The opt-in embedded MTR smoke runner now covers selected retained information
schema charset/collation/engine/schema/statistics/constraint metadata and an
additional optimizer regression. This is MariaDB embedded baseline coverage
only and does not change SQL, C API, storage-engine, file-format, or
server-surface policy.

## Design

- Add the selected passing tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Do not modify upstream MariaDB test files.
- Keep disabled file-I/O, native-engine, and result-shape candidates outside
  accepted coverage until they have their own policy or routed-storage tests.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice expands opt-in MariaDB embedded MTR
baseline coverage only.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe funcs_1.row_count_func funcs_1.is_character_sets funcs_1.is_collations funcs_1.is_coll_char_set_appl funcs_1.is_engines funcs_1.is_engines_memory funcs_1.is_schemata_embedded funcs_1.is_statistics_is funcs_1.is_table_constraints_is funcs_1.is_tables_embedded funcs_1.is_tables_is_embedded funcs_2.memory_charset funcs_2.myisam_charset optimizer_unfixed_bugs.bug49129`
- `tools/mylite-mtr-harness run funcs_1.is_character_sets funcs_1.is_collations funcs_1.is_coll_char_set_appl funcs_1.is_engines funcs_1.is_engines_memory funcs_1.is_schemata_embedded funcs_1.is_statistics_is funcs_1.is_table_constraints_is optimizer_unfixed_bugs.bug49129`
- `tools/mylite-mtr-harness coverage`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the nine selected passing tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Coverage inventory counts increase accepted upstream baseline coverage by
  nine files without changing known unsupported counts.
- No upstream MariaDB test files are modified for this slice.

## Verification Results

- `tools/mylite-mtr-harness probe funcs_1.row_count_func funcs_1.is_character_sets funcs_1.is_collations funcs_1.is_coll_char_set_appl funcs_1.is_engines funcs_1.is_engines_memory funcs_1.is_schemata_embedded funcs_1.is_statistics_is funcs_1.is_table_constraints_is funcs_1.is_tables_embedded funcs_1.is_tables_is_embedded funcs_2.memory_charset funcs_2.myisam_charset optimizer_unfixed_bugs.bug49129`: 9 passed, 5 failed, and 0 skipped.
- `tools/mylite-mtr-harness run funcs_1.is_character_sets funcs_1.is_collations funcs_1.is_coll_char_set_appl funcs_1.is_engines funcs_1.is_engines_memory funcs_1.is_schemata_embedded funcs_1.is_statistics_is funcs_1.is_table_constraints_is optimizer_unfixed_bugs.bug49129`: passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 432 accepted
  upstream baseline tests, 8 accepted MyLite profile tests, 19 accepted MyLite
  storage-routed tests, 459 accepted total tests, 4,615 known unsupported
  upstream tests, and 854 unclassified upstream tests.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- Broader `funcs_1` information-schema coverage still needs privilege and
  native-engine policy review.
- The accepted MEMORY engine metadata row is baseline metadata coverage, not
  storage-routed MEMORY row lifecycle coverage.
