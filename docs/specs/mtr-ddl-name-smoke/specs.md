# MTR DDL And Name Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.comment_column2`, `main.check`, `main.create_drop_db`,
`main.lowercase_utf8`, `main.key_diff`, `main.check_constraint_show`,
`main.constraints`, `main.create_drop_index`, `main.create-uca`, and
`main.create_w_max_indexes_64`. This adds curated embedded baseline coverage
for long column-comment metadata, `CHECK TABLE` behavior, database create/drop
existence options, lowercase UTF-8 table-name lookup, key comparison behavior
over different character-key lengths, CHECK-constraint `SHOW CREATE TABLE`
metadata, constraint syntax and enforcement, index create/drop variants, UCA
collation propagation through CTAS, and maximum-index DDL limits.

## Non-Goals

- Broad DDL, metadata, or identifier MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing result differences outside selected `SHOW CREATE TABLE`
  default-engine and Aria `PAGE_CHECKSUM=1` output.
- Treating server account or privilege behavior inside `main.check` as
  MyLite's final embedded account-management surface.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/comment_column2.test` covers long column comments
  and verifies table, column, and index comment metadata through
  information-schema queries.
- `mariadb/mysql-test/main/check.test` covers `CHECK TABLE` over ordinary
  tables, views, temporary tables, locked tables, and representative privilege
  checks.
- `mariadb/mysql-test/main/create_drop_db.test` covers
  `CREATE DATABASE IF NOT EXISTS`, `CREATE OR REPLACE DATABASE`, incompatible
  `OR REPLACE` plus `IF NOT EXISTS`, `DROP DATABASE IF EXISTS`, and expected
  duplicate/missing database diagnostics.
- `mariadb/mysql-test/main/lowercase_utf8.test` covers lower-case table-name
  behavior for a UTF-8 table identifier under the `lower_case_table_names=1`
  feature guard.
- `mariadb/mysql-test/main/key_diff.test` covers join and lookup behavior over
  different-length character keys.
- `mariadb/mysql-test/main/check_constraint_show.test` covers column-level and
  table-level CHECK constraints plus information-schema table-constraint
  visibility.
- `mariadb/mysql-test/main/constraints.test` covers CHECK constraint syntax,
  failed constraint enforcement, named UNIQUE constraints, stored-procedure
  ALTER paths, CTAS constraint preservation, and prepared statement table
  creation.
- `mariadb/mysql-test/main/create_drop_index.test` covers `CREATE INDEX IF NOT
  EXISTS`, `DROP INDEX IF EXISTS`, and `CREATE OR REPLACE INDEX` table
  metadata.
- `mariadb/mysql-test/main/create-uca.test` covers UCA collation and default
  propagation through `CREATE TABLE ... SELECT`.
- `mariadb/mysql-test/main/create_w_max_indexes_64.test` covers the 64-key
  limit and key-part/name limit diagnostics.
- All selected tests pass under the MyLite MTR smoke profile without upstream
  result-file changes. The CHECK, constraint, index, UCA, and maximum-index
  tests need narrow `SHOW CREATE TABLE` replacement rules because the smoke
  profile runs with Aria as the default storage engine and emits
  `PAGE_CHECKSUM=1`.
- Probed nearby DDL/name candidates stay outside this slice:
  - `main.comment_table`, `main.comment_column`, and `main.comment_index`
    needed profile-specific `PAGE_CHECKSUM=1` normalization and are now
    covered by [MTR comment DDL smoke](../mtr-comment-ddl-smoke/specs.md).
  - `main.comment_database` is skipped for embedded server.
  - `main.drop_bad_db_type` requires a debug build.
  - `main.create_windows` requires Windows.
  - `main.create-big` requires `--big-test`.
  - `main.create_w_max_indexes_128` requires a 128-index server build.
  - `main.create_or_replace_pfs` requires Performance Schema.
  - `main.enforce_storage_engine_opt` requires disabled native MyISAM startup
    behavior.
  - `main.function_defaults` reaches host-file SQL I/O through `OUTFILE`.
  - `main.identifier_partition` requires partitioning.
  - `main.column_compression_utf16` reaches zlib column compression, which is
    explicitly unsupported in the MyLite embedded profile.
  - `main.schema` reaches a deadlock-oriented multi-connection section and was
    interrupted rather than admitted as an embedded smoke candidate.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected DDL/name behavior including column comments, `CHECK TABLE`, database
create/drop existence options, lowercase UTF-8 table lookup, different-length
key comparisons, CHECK-constraint metadata, constraint syntax/enforcement,
index create/drop variants, UCA collation CTAS propagation, and maximum-index
limits. This remains curated MariaDB embedded baseline coverage, not broad
MTR-scale comparison and not MyLite storage-routing evidence.

## Design

- Add the selected DDL/name tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Add narrow upstream test-file `--replace_result` rules only before affected
  `SHOW CREATE TABLE` statements, mapping the smoke profile's `ENGINE=Aria` and
  `PAGE_CHECKSUM=1` output back to the upstream `ENGINE=MyISAM` expectation.
- Keep skipped, debug-only, platform-only, disabled-engine, unsupported-surface,
  hanging, and unrelated result-normalization candidates outside the default
  list.

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
- `tools/mylite-mtr-harness probe main.check_constraint_show main.constraints main.create_drop_index main.create-uca main.create_w_max_indexes_64`
- `tools/mylite-mtr-harness run main.comment_column2 main.check main.create_drop_db main.lowercase_utf8 main.key_diff main.check_constraint_show main.constraints main.create_drop_index main.create-uca main.create_w_max_indexes_64`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected DDL/name tests.
- All selected tests pass under the MyLite MTR smoke profile.
- Upstream MariaDB test-file changes are limited to explicit result
  normalization for the MTR smoke profile's default-engine output.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- Many nearby DDL tests appear mechanically admit-able only after additional
  profile-specific expected-result normalization; each should still be probed
  and documented separately before admission.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing DDL or metadata behavior.
