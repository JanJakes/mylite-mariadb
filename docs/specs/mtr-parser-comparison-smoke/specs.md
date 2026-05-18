# MTR Parser And Comparison Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.brackets`, `main.comments`, and `main.compare`. This adds curated
embedded baseline coverage for parenthesized query expressions, comment
parsing, executable comments, prepared-statement parse errors, string/integer
comparison behavior, binary/string comparisons, and selected comparison
predicates over table-backed rows.

## Non-Goals

- Running broad parser, optimizer, or SQL-mode MTR suites.
- Normalizing tests with many profile-sensitive engine result differences.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/brackets.test` exercises parenthesized UNION query
  expressions, nested ORDER/LIMIT tails, parenthesized table references,
  derived tables, CTAS and view creation from bracketed SELECTs, and selected
  EXPLAIN output.
- `mariadb/mysql-test/main/comments.test` exercises block comments, line
  comments, MariaDB and MySQL executable comments, version-gated comments, and
  prepared-statement parse failures for unterminated or malformed comments.
- `mariadb/mysql-test/main/compare.test` exercises string/integer comparison,
  trailing-space and embedded-NUL string comparison, binary comparisons,
  zerofill concatenation predicates, timestamp comparison predicates, and
  long-string indexed comparisons.
- Verified commands:
  - `tools/mylite-mtr-harness run main.brackets`
  - `tools/mylite-mtr-harness run main.comments`
  - `tools/mylite-mtr-harness run main.compare`
- Nearby candidates are not admitted in this slice:
  `main.comment_table` and `main.comment_column` need repeated Aria
  `PAGE_CHECKSUM=1` normalization, `main.comment_database` is skipped under
  embedded MTR, and `main.1st` depends on server-system tables omitted by the
  MyLite MTR smoke profile.
- `main.ansi` was intentionally left for a follow-up slice because it needed
  SQL-mode-specific default-engine normalization; it is now covered by
  [MTR ANSI and binary smoke](../mtr-ansi-binary-smoke/specs.md).

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected parser/comment and comparison behavior. This remains curated MariaDB
embedded baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

Add `main.brackets`, `main.comments`, and `main.compare` to
`tools/mylite-mtr-harness`'s default curated list. No MariaDB test-source
normalization is needed for these cases.

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
- `tools/mylite-mtr-harness run main.brackets`
- `tools/mylite-mtr-harness run main.comments`
- `tools/mylite-mtr-harness run main.compare`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.brackets`, `main.comments`, and
  `main.compare`.
- All three tests pass under the MyLite MTR smoke profile.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- These tests still run against MariaDB's embedded baseline, not MyLite routed
  storage. A later MTR-scale comparison slice must decide how to translate or
  replay accepted MTR SQL through MyLite's public embedded API.
