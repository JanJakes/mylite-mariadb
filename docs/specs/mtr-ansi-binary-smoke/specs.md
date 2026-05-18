# MTR ANSI And Binary Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with exact upstream tests
`main.ansi` and `main.binary`. This adds curated embedded baseline coverage for
ANSI SQL-mode expression behavior and broader binary string comparison,
collation, indexing, and `BINARY` column behavior.

## Non-Goals

- Broad SQL-mode, charset, or binary type MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Adding MTR to default compatibility harness groups.
- Normalizing skipped embedded-server, disabled-engine, Sequence, host-file
  I/O, or native-InnoDB cases.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/ansi.test` covers `sql_mode=ANSI`, `||`
  concatenation precedence, grouped SELECT behavior, and `SHOW CREATE TABLE`
  rendering under `MYSQL323`, `MYSQL40`, and `NO_FIELD_OPTIONS` modes.
- `mariadb/mysql-test/main/binary.test` covers binary and non-binary string
  comparisons, binary collations, indexed binary lookups, `BINARY` column
  default length rendering, padding, and binary-key trailing-byte behavior.
- Both tests pass under the MyLite MTR smoke profile after profile-specific
  default-engine text normalization for affected `SHOW CREATE TABLE` output.
- Probed nearby candidates stay outside this slice:
  - `main.alias`, `main.ctype_binary`, and `main.ctype_latin1` need broader
    Aria/MyISAM output normalization across long upstream result sections.
  - `main.binary_to_hex` and `main.temp_table` are skipped for embedded MTR.
  - `main.union` and `main.user_var` require the disabled Sequence engine.
  - `main.warnings` reaches host-file `LOAD DATA` coverage that MyLite rejects
    as a server-oriented file-I/O surface.
  - `main.create_or_replace` starts the embedded server with disabled native
    InnoDB information-schema options.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
ANSI SQL-mode expression behavior and broader binary string behavior in
addition to existing curated MTR smoke coverage. This remains MariaDB embedded
baseline coverage, not broad MTR-scale comparison and not MyLite storage
routing evidence.

## Design

- Add `main.ansi` and `main.binary` to `tools/mylite-mtr-harness`'s default
  curated list.
- Add profile-sensitive `--replace_result` directives immediately before the
  affected `SHOW CREATE TABLE` outputs in the selected upstream tests.
- Keep skipped, native-engine, Sequence, file-I/O, and wider normalization
  candidates outside the list.

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

- `tools/mylite-mtr-harness probe main.alias main.ansi main.binary main.binary_to_hex main.ctype_binary main.ctype_latin1 main.temp_table main.union main.user_var main.warnings main.create_or_replace`
- `tools/mylite-mtr-harness run main.ansi main.binary`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `main.ansi` and `main.binary`.
- Both tests pass under the MyLite MTR smoke profile.
- The only upstream test-source normalization is profile-specific default
  engine text normalization.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Risks And Open Questions

- This remains curated MariaDB embedded baseline coverage and does not prove
  MyLite storage-routing behavior for binary string indexes.
- Additional SQL-mode and charset suites need a separate normalization policy
  because many upstream tests include disabled engines, skipped embedded-server
  cases, host-file I/O, or server-only runtime assumptions.
