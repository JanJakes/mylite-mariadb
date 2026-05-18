# MTR Explain And UCA Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.explain`, `main.ctype_utf8mb3_general_ci`, and
`main.ctype_utf8mb4_uca1400_ai_ci`. This adds curated embedded baseline
coverage for ordinary EXPLAIN plan output and additional UTF-8 collation paths,
including UCA 1400 special-character, `WEIGHT_STRING`, regex, and LIKE
condition-propagation behavior.

## Non-Goals

- Broad optimizer, `ANALYZE`, optimizer-trace, or SHOW EXPLAIN coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Admitting nearby tests that require native MyISAM/InnoDB, Sequence, special
  build options, server-only embedded skips, or host-file/disk-temp behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/explain.test` covers basic EXPLAIN output,
  key-hint error handling, `EXPLAIN EXTENDED`, prepared EXPLAIN statements,
  derived-table and subquery EXPLAIN regressions, EXPLAIN over REPLACE, DELETE,
  UNION, VALUES, views, full-text metadata, and encoded identifiers.
- `mariadb/mysql-test/main/ctype_utf8mb3_general_ci.test` covers
  `utf8mb3_general_ci` special-character comparisons, `WEIGHT_STRING`,
  regex behavior, UTF-8 regex behavior, and LIKE constant-condition
  propagation.
- `mariadb/mysql-test/main/ctype_utf8mb4_uca1400_ai_ci.test` covers the same
  charset-expression surfaces for the MariaDB 11.8 UCA 1400
  `uca1400_ai_ci` collation.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed nearby candidates remain outside this slice:
  - `main.comment_database` is skipped for embedded server.
  - `main.count_distinct2` depends on status output affected by absent CSV
    disk-temp fallback.
  - `main.count_distinct3` requires `--big-test`.
  - `main.create_w_max_indexes_128` requires a special build option.
  - Several broader legacy charset tests switch to native MyISAM or include
    MyISAM-specific all-engine coverage.
  - `main.ctype_utf8mb4_unicode_ci_casefold` requires the Sequence engine.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected EXPLAIN plan output and additional UTF-8 UCA 1400 collation behavior.
This remains curated MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add the selected tests to `tools/mylite-mtr-harness`'s default curated list.
- Do not modify upstream MariaDB test files for this slice.
- Keep nearby tests with native-engine assumptions, special build requirements,
  server-only skips, or host-file/disk-temp behavior outside the curated list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.explain main.ctype_utf8mb3_general_ci main.ctype_utf8mb4_uca1400_ai_ci`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No upstream MariaDB test files are modified.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- `main.explain` is plan-output coverage over MariaDB's optimizer and handler
  estimates. It does not prove MyLite routed-storage plan quality.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing behavior.
