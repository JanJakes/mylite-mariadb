# Procedure Analyse Trim

## Problem Statement

MyLite's default embedded profile should keep application-facing SQL behavior
while trimming legacy diagnostic extensions that do not fit the core library
contract. `SELECT ... PROCEDURE ANALYSE()` is an obsolete result-set analysis
extension wired through MariaDB's generic SELECT procedure runtime. It is not
JSON, GEOMETRY/GIS, native storage, DDL, DML, transactions, or the public C API.

This slice omits the `PROCEDURE ANALYSE()` implementation from the embedded
archive and makes the unsupported surface explicit through public SQL policy
coverage.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses a SELECT `PROCEDURE` clause through
  `procedure_clause`.
- `mariadb/sql/sql_select.cc` calls `setup_procedure()` when a SELECT carries
  procedure parameters, then routes rows through the returned `Procedure`
  object.
- `mariadb/sql/procedure.cc` registers the built-in `analyse` SELECT procedure
  and dispatches it to `proc_analyse_init()`.
- `mariadb/sql/sql_analyse.cc` and `mariadb/sql/sql_analyse.h` implement the
  `PROCEDURE ANALYSE()` result analyzer and its supporting field analysis
  classes.
- `mariadb/libmysqld/CMakeLists.txt` includes `../sql/sql_analyse.cc` in the
  embedded SQL archive source list.
- Historical bundle-size research identifies omitting `sql_analyse.cc` as an
  archive-only win with no linked-smoke regression. The exact saving must be
  rerun against the current baseline before being accepted.

## Proposed Design

Add a MyLite-owned `MYLITE_WITH_PROCEDURE_ANALYSE` CMake option that defaults
to `ON`, preserving normal MariaDB server behavior. The MyLite embedded
baseline sets the option to `OFF`.

When the option is `OFF`, `mariadb/libmysqld/CMakeLists.txt` removes
`../sql/sql_analyse.cc` from `sql_embedded` and links a small
`mylite_procedure_analyse_stub.cc` source that provides `proc_analyse_init()`.
The stub reports `ER_NOT_SUPPORTED_YET` and returns `nullptr`, so the retained
generic SELECT procedure dispatch remains fail-closed if a query bypasses the
public MyLite policy.

Add a narrow pre-dispatch policy check for `SELECT ... PROCEDURE ANALYSE()` in
`libmylite`. The policy is not a general SQL parser; it recognizes the
straightforward top-level clause shape before direct execution or prepared
statement preparation. Quoted mentions such as string literals remain normal
SQL. More complex or quoted procedure spellings still fail through MariaDB's
retained dispatch and the embedded stub.

## Affected MariaDB Subsystems

- Embedded `sql_embedded` archive source selection.
- SELECT procedure analyzer implementation in `sql_analyse.cc`.
- Public MyLite SQL policy for unsupported optional diagnostics.

The generic SELECT procedure runtime in `procedure.cc` remains linked in this
slice. Removing it is a broader follow-up because the dispatch path is shared
with SELECT planning and should be measured separately.

## Compatibility Impact

`PROCEDURE ANALYSE()` becomes explicitly unsupported in the default embedded
profile. Ordinary SELECT execution, query-cache SELECT hints, built-in SQL
functions, JSON, GEOMETRY/GIS, native storage engines, DDL, DML, transactions,
and prepared statements remain supported according to their existing status.

This is a diagnostic extension, not core MySQL/MariaDB application data
behavior. The compatibility matrix records the unsupported surface.

## DDL Metadata Routing Impact

None. `PROCEDURE ANALYSE()` does not create or alter table metadata.

## Database-Directory And Lifecycle Impact

None. The slice does not change `datadir/`, `tmp/`, `run/`, locking, recovery,
or native engine file placement.

## Public API Impact

No symbols or headers change. Direct execution and prepared preparation return
`MYLITE_ERROR` with a stable diagnostic when the policy rejects
`PROCEDURE ANALYSE()`.

## Native Storage Impact

None. InnoDB, MyISAM, Aria, and MEMORY remain part of the embedded profile.

## Binary-Size Impact

This is an archive-only reduction from omitting `sql_analyse.cc.o` and adding a
much smaller stub object. On the current branch, `tools/mariadb-embedded-build
all` reports `libmariadbd.a` at 27,226,608 bytes / 25.97 MiB with 705 archive
members. That is 39,120 bytes smaller than the previous 27,265,728-byte
baseline, with no member-count change because the implementation object is
replaced by the stub.

## License Or Dependency Impact

No new dependencies or license changes. The stub is GPL-2.0-compatible
first-party MyLite code linked into the GPL-2.0 MariaDB-derived archive.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `sql_analyse.cc.o` is absent and
  `mylite_procedure_analyse_stub.cc.o` is present in `libmariadbd.a`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`.
- Run `ctest --preset dev --output-on-failure`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The default embedded baseline sets `MYLITE_WITH_PROCEDURE_ANALYSE=OFF`.
- Normal MariaDB builds keep the option defaulted to `ON`.
- The embedded archive omits `sql_analyse.cc.o`.
- Direct and prepared `SELECT ... PROCEDURE ANALYSE()` fail predictably through
  MyLite policy.
- A quoted literal mentioning `PROCEDURE ANALYSE()` still executes normally.
- Compatibility, API, roadmap, and build-profile docs describe the unsupported
  diagnostic surface.
- Current archive size and member count are recorded.

## Risks And Unresolved Questions

- The public SQL policy is intentionally narrow and is not a full SQL parser.
  The embedded MariaDB stub remains the correctness backstop for bypassed or
  unusual forms.
- The generic SELECT procedure runtime remains linked. A later trim may remove
  it, but that should be a separate slice with fresh source and compatibility
  review.
