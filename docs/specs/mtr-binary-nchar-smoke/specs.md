# MTR Binary And NCHAR Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.type_binary` and `main.type_nchar`. This adds curated embedded baseline
coverage for binary/varbinary comparison and padding semantics plus national
character type aliases.

## Non-Goals

- Broad type MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Admitting wider integer, decimal, enum, set, NULL, grouping, or function
  suites that need disabled native engines, skipped embedded features, or wider
  result normalization.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_binary.test` covers `BINARY` and `VARBINARY`
  padding, trailing-byte comparison, indexed and non-indexed search, distinct
  comparison, casts, truncation diagnostics, and BLOB/VARBINARY decimal casts.
- `mariadb/mysql-test/main/type_nchar.test` covers `NCHAR`, `NVARCHAR`, and
  `NATIONAL CHARACTER` aliases through `SHOW CREATE TABLE`.
- Both selected tests pass under the MyLite MTR smoke profile after the same
  profile-specific `ENGINE=Aria` / `PAGE_CHECKSUM=1` `SHOW CREATE TABLE`
  normalization already used by existing curated MTR tests.
- Probed nearby candidates stay outside this slice:
  - `main.type_int` needs broader stored-procedure result normalization.
  - `main.type_bit`, `main.type_bool`, `main.type_float`, `main.type_enum`,
    `main.type_set`, `main.null`, `main.null_key`, `main.func_if`,
    `main.func_set`, `main.func_date_add`, `main.having`, and
    `main.insert_select` depend on disabled native MyISAM or other profile
    differences.
  - `main.type_decimal`, `main.type_newdecimal`, `main.func_math`,
    `main.func_hybrid_type`, and `main.empty_string_literal` reach trimmed GIS
    or profile-specific create/select behavior.
  - `main.type_blob`, `main.group_min_max`, and related suites require native
    InnoDB startup options disabled in the MyLite MTR profile.
  - `main.func_misc`, `main.func_str`, `main.bool`, `main.distinct`,
    `main.group_by`, `main.strings`, `main.create_select`, and
    `main.mysqltest_string_functions` are skipped under the current embedded
    profile or require disabled engines.
  - `main.count_distinct2` and `main.select_safe` need separate result
    normalization review.
  - `main.type_varchar` needed separate result normalization review and is
    now covered by [MTR VARCHAR smoke](../mtr-varchar-smoke/specs.md).

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected binary/varbinary byte semantics and national-character type aliases in
addition to existing curated type coverage. This remains MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite
storage-routing evidence.

## Design

- Add `main.type_binary` and `main.type_nchar` to
  `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in the selected upstream tests.
- Keep failed, skipped, disabled-engine, unsupported-surface, and
  normalization-heavy candidates outside the list.

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
- `tools/mylite-mtr-harness run main.type_binary main.type_nchar`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.type_binary` and `main.type_nchar`.
- Both tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This remains curated MariaDB embedded baseline coverage and does not prove
  MyLite storage-routing behavior for binary or national character columns.
- More type suites are likely admissible only after a separate normalization
  policy for stored-procedure result sets, disabled engines, and trimmed
  subsystem references.
