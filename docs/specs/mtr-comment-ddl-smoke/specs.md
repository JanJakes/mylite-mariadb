# MTR Comment DDL Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.comment_column`, `main.comment_table`, and `main.comment_index`. This
adds curated embedded baseline coverage for table, column, and index comment
metadata DDL and information-schema visibility.

## Non-Goals

- Broad DDL, metadata, or information-schema MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing skipped embedded-server, replication, Performance Schema, native
  InnoDB, or broader create-or-replace suites.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/comment_column.test` covers column comment
  publication, ALTER add/modify/change/drop behavior, overflow warnings and
  errors, and table/column/index comment visibility through information
  schema.
- `mariadb/mysql-test/main/comment_table.test` covers table comment length
  boundaries, warning/error behavior, invalid character handling, and `SHOW
  CREATE TABLE` rendering.
- `mariadb/mysql-test/main/comment_index.test` covers index comment
  publication through CREATE/ALTER/DROP index paths, multicolumn index
  comments, overflow warnings and errors, and `SHOW CREATE TABLE` rendering.
- All selected tests pass under the MyLite MTR smoke profile after extending
  their existing `$ENGINE` result normalization to also strip Aria's
  profile-specific `PAGE_CHECKSUM=1` table option.
- Probed nearby candidates stay outside this slice:
  - `main.comment_database` and `main.create_or_replace_permission` are skipped
    for embedded MTR.
  - `main.create_or_replace2` is skipped because the MTR smoke runner skips
    replication suites.
  - `main.create_or_replace_pfs` requires Performance Schema.
  - `main.create_or_replace` starts the embedded server with disabled native
    InnoDB information-schema options.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected DDL/name and comment metadata behavior. This remains MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite storage
routing evidence.

## Design

- Add `main.comment_column`, `main.comment_table`, and `main.comment_index` to
  `tools/mylite-mtr-harness`'s default curated list.
- Extend the selected upstream tests' existing `--replace_result $ENGINE
  ENGINE` directives with `" PAGE_CHECKSUM=1" ""`.
- Keep skipped, replication, Performance Schema, native-InnoDB, and wider
  create-or-replace candidates outside the list.

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

- `tools/mylite-mtr-harness probe main.comment_column main.comment_table main.comment_index main.comment_database main.create_or_replace2 main.create_or_replace_permission main.create_or_replace_pfs`
- `tools/mylite-mtr-harness run main.comment_column main.comment_table main.comment_index`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes all three comment DDL tests.
- All three tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific
  `PAGE_CHECKSUM=1` output normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This remains curated MariaDB embedded baseline coverage and does not prove
  MyLite storage-routing behavior for comment metadata.
- Broader create-or-replace, information-schema, and native-engine DDL suites
  still need explicit normalization or unsupported-surface policy before
  promotion.
