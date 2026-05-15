# SELECT PROCEDURE Runtime Trim

## Problem

The default MyLite embedded profile still builds MariaDB's generic
`SELECT ... PROCEDURE` dispatch runtime in `procedure.cc`. The only built-in
SELECT procedure, `PROCEDURE ANALYSE()`, is already explicitly unsupported and
the default embedded profile now links a MyLite stub instead of the full
`sql_analyse.cc` analyser.

The remaining dispatch layer is an embedded-hostile extension hook with no
supported MyLite caller. This slice removes it from the default embedded
profile while preserving the parser and a clear unsupported diagnostic for
internal callers that bypass `libmylite` SQL-policy checks.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documents the `PROCEDURE` SELECT clause as server-side result-set
  post-processing through C procedures, and says `ANALYSE` is currently the
  only available procedure:
  <https://mariadb.com/docs/server/reference/sql-statements/data-manipulation/selecting-data/procedure>.
- `docs/specs/procedure-analyse-trim/specs.md` already makes
  `PROCEDURE ANALYSE()` explicitly unsupported for MyLite and verifies public
  direct/prepared APIs reject it before MariaDB execution.
- `mariadb/sql/sql_yacc.yy:13241` parses `PROCEDURE_SYM ident`, stores the
  procedure name and arguments in `LEX::proc_list`, and marks the SELECT with
  `OPTION_PROCEDURE_CLAUSE`.
- `mariadb/sql/procedure.h:178` defines the `Procedure` interface and
  `mariadb/sql/procedure.h:196` declares `setup_procedure()`.
- `mariadb/sql/procedure.cc:27` defines the built-in procedure dispatch table,
  `mariadb/sql/procedure.cc:39` maps `analyse` to `proc_analyse_init()`, and
  `mariadb/sql/procedure.cc:79` implements `setup_procedure()`.
- `mariadb/sql/sql_select.cc:1820` calls `setup_procedure()` during SELECT
  preparation and only enters the procedure-specific field, grouping, and
  ordering checks when a `Procedure *` is returned.
- The current default embedded archive contains both `procedure.cc.o` and
  `mylite_procedure_analyse_stub.cc.o`; exported symbols include
  `setup_procedure()`, `proc_analyse_init()`, and the `Item_proc_*` helper
  methods from `procedure.cc`.
- The normal `mariadb/sql/CMakeLists.txt:175` server SQL target also links
  `procedure.cc`; this slice only changes the embedded profile source list.

## Design

- Add `MYLITE_WITH_SELECT_PROCEDURE_RUNTIME`, defaulting to `ON` for
  upstream-compatible build behavior.
- Set `MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, replace
  `../sql/procedure.cc` in `SQL_EMBEDDED_SOURCES` with
  `mylite_select_procedure_stub.cc`.
- When the generic SELECT procedure runtime is disabled, omit
  `mylite_procedure_analyse_stub.cc` too. The analyser entry point is only
  referenced by the removed dispatch table.
- Keep parser syntax intact. The replacement stub implements
  `setup_procedure()`: it returns no procedure and no error when no clause is
  present, and reports `ER_NOT_SUPPORTED_YET` when a parsed procedure clause is
  present.
- Add a generic `libmylite` SQL-policy rejection for parsed-looking
  `SELECT ... PROCEDURE name(...)` clauses so public direct and prepared APIs
  fail before MariaDB execution with stable MyLite diagnostics. Keep the
  existing specific `PROCEDURE ANALYSE()` diagnostic for the known MariaDB
  built-in clause.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- SELECT procedure-clause setup.
- Embedded direct and prepared SQL diagnostics tests.
- Compatibility, size-profile, and roadmap documentation.

## MySQL/MariaDB Compatibility Impact

The default MyLite embedded profile continues to mark `SELECT ... PROCEDURE`
as out of scope. Public behavior for the known built-in clause remains the same:
`SELECT ... PROCEDURE ANALYSE()` is rejected by MyLite before MariaDB
execution.

Internal embedded callers that bypass MyLite SQL policy still get a MariaDB
diagnostic from the stub instead of accidentally reaching a removed symbol. The
normal MariaDB SQL target keeps the full `procedure.cc` implementation for
comparison builds.

## DDL Metadata Routing Impact

None. The `PROCEDURE` SELECT clause post-processes result rows and does not
publish table definitions, row data, indexes, constraints, or MyLite catalog
records.

## Single-File And Embedded-Lifecycle Impact

Removing `procedure.cc` does not create new files and does not alter database
open/close lifetime. The unsupported path must not allocate long-lived
procedure state or touch server metadata tables.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Existing public
`PROCEDURE ANALYSE()` rejection remains `MYLITE_ERROR`, SQLSTATE `HY000`,
MariaDB errno `0`, and a message containing `PROCEDURE ANALYSE`.

## Storage-Engine Routing Impact

None. Ordinary SELECT execution over routed MyLite tables remains available;
only the unsupported result-set procedure hook is removed.

## Wire-Protocol Or Integration-Package Impact

Core `libmylite` rejects the SELECT procedure clause before execution. A future
wire-protocol adapter should preserve this unsupported policy unless a real
application dependency appears.

## Binary-Size Impact

The implemented trim should remove `procedure.cc.o`, the `Item_proc_*` helper
methods, and the now-unreferenced `mylite_procedure_analyse_stub.cc.o` from the
embedded archives, replacing them with a small `setup_procedure()` stub. The
bundle-size research estimated this as a meaningful archive cleanup with a
small linked-binary win because ordinary SELECT setup remains live.

Current baseline before this slice:

| Artifact | Bytes | Members |
| --- | ---: | ---: |
| Default `libmariadbd.a` | 28,160,056 | 689 |
| Storage-smoke `libmariadbd.a` | 28,340,640 | 692 |

## License And Dependency Impact

No new dependency. The replacement stub is GPL-2.0-compatible first-party
MyLite code inside the GPL-2.0 MariaDB-derived tree.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both embedded archives contain `mylite_select_procedure_stub.cc.o`
  and no `procedure.cc.o` or `mylite_procedure_analyse_stub.cc.o`.
- Confirm `setup_procedure()` remains defined and the full `Item_proc_*`
  helper symbols are absent from both archives.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full SELECT procedure runtime.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- The default embedded cache records
  `MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF`.
- Direct and prepared `SELECT ... PROCEDURE ANALYSE()` and representative
  generic `SELECT ... PROCEDURE name()` clauses are rejected by `libmylite`
  before MariaDB execution.
- Quoted text containing `PROCEDURE ANALYSE` remains accepted.
- The embedded archive links the MyLite `setup_procedure()` stub instead of
  full `procedure.cc`.
- Documentation and compatibility matrix mark the generic SELECT procedure
  runtime explicitly unsupported for the default embedded profile.
- Size measurements are recorded.

## Risks And Unresolved Questions

- Parser syntax remains present, so internal callers that bypass `libmylite`
  can still parse `SELECT ... PROCEDURE ...` and receive the stub diagnostic
  during SELECT preparation.
- This removes the generic SELECT procedure extension hook, not just
  `ANALYSE`. That is acceptable for the default embedded profile because
  MariaDB exposes no other built-in SELECT procedures and MyLite does not
  support dynamic server-side procedure hooks.

## Implementation Results

Implemented with `MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF` in the default
embedded profile. `mariadb/libmysqld/CMakeLists.txt` now links
`mylite_select_procedure_stub.cc` in place of `../sql/procedure.cc` for
`sql_embedded` and omits the now-unreferenced `PROCEDURE ANALYSE()` stub when
the generic runtime is disabled. The normal `sql` target still builds
`procedure.cc`.

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
  `mylite_select_procedure_stub.cc.o`;
- neither archive includes `procedure.cc.o` or
  `mylite_procedure_analyse_stub.cc.o`;
- `setup_procedure()` remains as a stub entry point;
- `proc_analyse_init()` and full `Item_proc_*` helper symbols are absent from
  both archives.

Measured size impact compared with the previous `PROCEDURE ANALYSE()` baseline:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| Default `libmariadbd.a` | 28,117,968 | -42,088 |
| Storage-smoke `libmariadbd.a` | 28,298,552 | -42,088 |
| Embedded open-close smoke | 17,936,496 | -2,928 |
| Embedded exec smoke | 17,954,376 | -2,928 |
| Embedded statement smoke | 17,952,688 | -2,992 |
| Embedded warning smoke | 17,936,096 | -2,960 |
| Embedded comparison smoke | 18,042,816 | -2,976 |
| Storage-smoke open-close smoke | 18,015,104 | -2,928 |
| Storage-smoke exec smoke | 18,049,512 | -2,928 |
| Storage-smoke statement smoke | 18,047,856 | -2,944 |
| Storage-smoke warning smoke | 18,014,704 | -2,976 |
| Storage-smoke comparison smoke | 18,116,944 | -2,976 |
| Storage-engine smoke | 18,283,792 | -2,944 |
