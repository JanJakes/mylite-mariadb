# PROCEDURE ANALYSE Trim

## Problem

The default MyLite embedded profile still builds MariaDB's
`PROCEDURE ANALYSE()` implementation. This is a legacy server-side result-set
analysis feature for interactive schema tuning, not part of MyLite's embedded
file-owned runtime contract. It also sits in a relatively isolated
`sql_analyse.cc` object.

This slice removes the full `PROCEDURE ANALYSE()` implementation from the
default embedded archive while preserving an explicit unsupported-feature
diagnostic.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documents `PROCEDURE ANALYSE()` as a result-set analyser that suggests
  column data types and says it is defined in `sql/sql_analyse.cc`:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/information-functions/procedure-analyse>.
- MariaDB documents the `PROCEDURE` SELECT clause as server-side result-set
  post-processing through C procedures, and says `ANALYSE` is currently the
  only available procedure:
  <https://mariadb.com/docs/server/reference/sql-statements/data-manipulation/selecting-data/procedure>.
- `mariadb/sql/sql_yacc.yy:13241` parses `PROCEDURE_SYM ident`, stores the
  procedure name and arguments in `LEX::proc_list`, and marks the SELECT with
  `OPTION_PROCEDURE_CLAUSE`.
- `mariadb/sql/procedure.cc:27` defines the built-in procedure dispatch table,
  and `mariadb/sql/procedure.cc:39` maps `analyse` to `proc_analyse_init()`.
- `mariadb/sql/sql_analyse.h:62` declares `proc_analyse_init()`.
- `mariadb/sql/sql_analyse.cc:95` implements `proc_analyse_init()`, and the
  rest of `sql_analyse.cc` implements `analyse` and `field_info` helpers for
  scanning and reshaping result-set data.
- `mariadb/sql/sql_select.cc:1820` calls `setup_procedure()` during SELECT
  preparation when a procedure clause is present.
- The embedded source list in `mariadb/libmysqld/CMakeLists.txt` links
  `../sql/sql_analyse.cc`. The normal `mariadb/sql/CMakeLists.txt:180` server
  SQL target also links `sql_analyse.cc`, but this slice only changes the
  embedded profile.

## Design

- Add `MYLITE_WITH_PROCEDURE_ANALYSE`, defaulting to `ON` for
  upstream-compatible build behavior.
- Set `MYLITE_WITH_PROCEDURE_ANALYSE=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, replace
  `../sql/sql_analyse.cc` in `SQL_EMBEDDED_SOURCES` with
  `mylite_procedure_analyse_stub.cc`.
- Keep parser syntax and generic `procedure.cc` dispatch intact. The stub
  implements `proc_analyse_init()` and returns `ER_NOT_SUPPORTED_YET`.
- Add a `libmylite` SQL-policy rejection for statements containing the
  `PROCEDURE ANALYSE` SELECT clause so public direct and prepared APIs fail
  before MariaDB execution with stable MyLite diagnostics.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- SELECT procedure post-processing.
- `libmylite` unsupported SQL policy.
- Embedded direct and prepared SQL diagnostics tests.
- Compatibility harness server-surface documentation.
- Size-profile documentation and roadmap status.

## MySQL/MariaDB Compatibility Impact

`PROCEDURE ANALYSE()` moves to explicit out-of-scope status for the default
MyLite embedded profile. MyLite preserves ordinary SELECT execution, result
fetching, and expression evaluation while omitting the legacy server-side
result-set analysis clause.

The normal MariaDB SQL target keeps the full implementation for comparison
builds.

## DDL Metadata Routing Impact

None. `PROCEDURE ANALYSE()` reads a SELECT result set and does not publish user
table definitions, row data, indexes, constraints, or MyLite catalog records.

## Single-File And Embedded-Lifecycle Impact

Removing `sql_analyse.cc` does not create new files and does not alter
database open/close lifetime. The unsupported path must not allocate long-lived
procedure state or open server metadata tables.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. The public behavior is a stable
`MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB errno `0`, and message containing
`PROCEDURE ANALYSE` when a direct or prepared public API receives the clause.

## Storage-Engine Routing Impact

None. Tests can use ordinary temporary tables to reach the SELECT clause, but
the feature itself does not route through a storage engine.

## Wire-Protocol Or Integration-Package Impact

Core `libmylite` rejects `PROCEDURE ANALYSE()`. A future CLI or compatibility
adapter should keep this unsupported unless a clear application dependency
appears.

## Binary-Size Impact

The full `sql_analyse.cc` object contains the only built-in SELECT procedure
implementation. The implemented trim removes 40,760 bytes from both measured
embedded archives and reduces linked smoke binaries where the linker previously
retained analyser code.

## License And Dependency Impact

No new dependency. The replacement stub is GPL-2.0-compatible first-party
MyLite code inside the GPL-2.0 MariaDB-derived tree.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm `libmariadbd.a` contains `mylite_procedure_analyse_stub.cc.o` and no
  `sql_analyse.cc.o`.
- Confirm full analyser helper symbols are absent from default and
  storage-smoke archives.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full `PROCEDURE ANALYSE()`.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- The default embedded cache records `MYLITE_WITH_PROCEDURE_ANALYSE=OFF`.
- Direct and prepared `SELECT ... PROCEDURE ANALYSE()` are rejected by
  `libmylite` before MariaDB execution.
- Quoted text containing `PROCEDURE ANALYSE` remains accepted.
- The embedded archive links the MyLite stub instead of full `sql_analyse.cc`.
- Documentation and compatibility matrix mark `PROCEDURE ANALYSE()` explicitly
  unsupported for the default embedded profile.
- Size measurements are recorded.

## Risks And Unresolved Questions

- The generic `PROCEDURE` clause dispatch remains compiled because it is tied
  into SELECT preparation. A follow-up slice can remove that dispatch after the
  sole built-in implementation is gone.
- Parser syntax remains present, so internal callers that bypass `libmylite`
  can still parse `PROCEDURE ANALYSE()` and receive the stub diagnostic.

## Implementation Results

Implemented with `MYLITE_WITH_PROCEDURE_ANALYSE=OFF` in the default embedded
profile. `mariadb/libmysqld/CMakeLists.txt` now links
`mylite_procedure_analyse_stub.cc` in place of `../sql/sql_analyse.cc` for
`sql_embedded`; the normal `sql` target still builds `sql_analyse.cc`.

Verification:

```sh
tools/mariadb-embedded-build configure
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset storage-smoke-dev
ctest --preset storage-smoke-dev --output-on-failure
tools/mylite-compat-harness report server-surface
tools/mylite-size-report
cmake --build build/mariadb-embedded --target sql
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report
git diff --check
cmake --build --preset dev --target tidy
```

Observed archive contents:

- both default and storage-smoke archives include
  `mylite_procedure_analyse_stub.cc.o`;
- neither archive includes `sql_analyse.cc.o`;
- full analyser helper symbols are absent from both archives;
- `proc_analyse_init()` remains as a stub entry point.

Measured size impact compared with the previous SQL `HELP` baseline:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| Default `libmariadbd.a` | 28,160,056 | -40,760 |
| Storage-smoke `libmariadbd.a` | 28,340,640 | -40,760 |
| Embedded open-close smoke | 17,939,424 | -22,640 |
| Embedded exec smoke | 17,957,304 | -22,496 |
| Embedded statement smoke | 17,955,680 | -39,136 |
| Embedded warning smoke | 17,939,056 | -22,640 |
| Embedded comparison smoke | 18,045,792 | -22,608 |
| Storage-smoke open-close smoke | 18,018,032 | -22,640 |
| Storage-smoke exec smoke | 18,052,440 | -22,480 |
| Storage-smoke statement smoke | 18,050,800 | -22,624 |
| Storage-smoke warning smoke | 18,017,680 | -39,136 |
| Storage-smoke comparison smoke | 18,119,920 | -22,592 |
| Storage-engine smoke | 18,286,736 | -22,624 |
