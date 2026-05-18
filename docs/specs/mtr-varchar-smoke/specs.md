# MTR VARCHAR Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream test
`main.type_varchar`. This adds curated embedded baseline coverage for
`VARCHAR`/`VARBINARY` comparison, indexing, conversion, ALTER, and legacy
old-VARCHAR upgrade behavior.

## Non-Goals

- Broad type MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Treating the test's copied `.frm` fixture as a MyLite storage design.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/type_varchar.test` covers ordinary `VARCHAR`,
  `VARBINARY`, and `TEXT` comparison semantics, primary-key conversion,
  varchar-to-text ALTER paths, long varchar definitions, and old-VARCHAR
  fixture upgrade behavior through `std_data/vchar.frm`.
- The test also covers a FULLTEXT addition over the upgraded old-VARCHAR
  fixture under the MTR profile's default storage engine.
- The selected test passes under the MyLite MTR smoke profile after the same
  profile-specific `ENGINE=Aria` / `PAGE_CHECKSUM=1` `SHOW CREATE TABLE`
  normalization already used by existing curated MTR tests.
- Probed nearby candidates from the previous MTR type slice remain outside this
  slice when they need disabled native engines, skipped embedded features,
  native InnoDB startup options, trimmed GIS references, plan/status
  normalization, or broader stored-procedure result normalization.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected VARCHAR/VARBINARY type behavior in addition to existing curated type
coverage. This remains MariaDB embedded baseline coverage, not broad MTR-scale
comparison and not MyLite storage-routing evidence.

## Design

- Add `main.type_varchar` to `tools/mylite-mtr-harness`'s default curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in the upstream test.
- Keep the fixture-backed old-VARCHAR coverage as MariaDB embedded baseline
  evidence only; it does not imply MyLite will use `.frm` files for storage.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The test runs
inside `build/mariadb-mtr-smoke/mysql-test/var` and may copy its upstream
fixture there as part of MariaDB MTR execution.

## Embedded Lifecycle And API

No `libmylite` API change. The slice only expands opt-in MariaDB embedded MTR
baseline coverage.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run main.type_varchar`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.type_varchar`.
- The test passes under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This remains curated MariaDB embedded baseline coverage and does not prove
  MyLite storage-routing behavior for `VARCHAR`, `VARBINARY`, `TEXT`, or
  FULLTEXT paths.
- The copied old-VARCHAR `.frm` fixture belongs to upstream MTR only; MyLite's
  product storage model still rejects durable MariaDB sidecars.
