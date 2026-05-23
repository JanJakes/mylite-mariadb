# MTR FULLTEXT Baseline Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.gcc296`,
`main.fulltext_multi`, `main.fulltext_charsets`, `main.fulltext_update`, and
`main.fulltext_cache`. This adds curated embedded baseline coverage for
selected FULLTEXT declarations, multiple FULLTEXT indexes, `MATCH ... AGAINST`
execution, UTF-8 fulltext search edge behavior, FULLTEXT index updates, table
checks, and join/cache search ordering over the MTR profile's Aria-backed
scratch tables.

## Non-Goals

- Implementing MyLite routed-storage FULLTEXT indexes.
- Running these tests through MyLite routed storage.
- Enabling native MyISAM in the embedded profile.
- Broad FULLTEXT, parser, optimizer, or charset MTR promotion.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/gcc296.test` creates a table with ordinary unique
  and FULLTEXT indexes over `varchar` / `tinytext` columns, inserts rows, and
  verifies row visibility.
- `mariadb/mysql-test/main/fulltext_multi.test` creates multiple FULLTEXT
  indexes over one table and verifies `MATCH ... AGAINST` scoring for single
  and combined indexed columns.
- `mariadb/mysql-test/main/fulltext_charsets.test` exercises an `utf8mb4`
  FULLTEXT index over combining marks and boolean fulltext search.
- `mariadb/mysql-test/main/fulltext_update.test` updates rows covered by a
  multi-column FULLTEXT index and checks the table afterward.
- `mariadb/mysql-test/main/fulltext_cache.test` verifies `MATCH ... AGAINST`
  scoring and ordering across join orders.
- All five tests pass unmodified under the MyLite MTR smoke profile, which
  uses Aria as the default storage engine and keeps native MyISAM disabled.

## Design

- Add the three passing upstream tests to the curated baseline MTR smoke list
  near related DDL/index coverage.
- Do not patch upstream test sources or expected result files.
- Keep documentation explicit that this is MariaDB/Aria baseline MTR smoke
  coverage, not MyLite handler FULLTEXT support.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected baseline evidence for
FULLTEXT syntax and Aria fulltext execution. MyLite routed storage still
explicitly rejects unsupported FULLTEXT index definitions before catalog
publication.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The tests run in the MTR smoke vardir and
use the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. Existing storage-routed compatibility tests remain the
authority for MyLite handler behavior, including FULLTEXT rejection.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.gcc296 main.fulltext_charsets main.fulltext_multi main.fulltext_update main.fulltext_cache`
- `tools/mylite-mtr-harness run main.gcc296 main.fulltext_charsets main.fulltext_multi main.fulltext_update main.fulltext_cache`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- All five selected tests report MTR passes under strict harness execution.
- The default MTR smoke list includes the selected FULLTEXT baseline tests.
- No `.reject` files remain.
- Docs keep routed-storage FULLTEXT support explicitly out of scope.

## Verification Results

- `tools/mylite-mtr-harness probe main.gcc296 main.fulltext_charsets main.fulltext_multi`:
  passed.
- `tools/mylite-mtr-harness probe main.fulltext_update main.fulltext_cache`:
  passed.
- `tools/mylite-mtr-harness run main.gcc296 main.fulltext_charsets main.fulltext_multi main.fulltext_update main.fulltext_cache`:
  passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 200
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `208`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The tests exercise MariaDB/Aria baseline behavior only. MyLite storage should
continue to reject FULLTEXT index definitions until a dedicated routed-storage
FULLTEXT design exists.
