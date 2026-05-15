# Stored Program Runtime Trim

## Problem

The default MyLite embedded profile rejects view, trigger, routine, package,
sequence, and `CALL` statements through the public `libmylite` SQL policy, but
the embedded archive still carries MariaDB's stored-program compiler and
runtime objects.

Stored programs currently have no MyLite-owned catalog, persistence format,
recovery model, or single-file lifecycle. Retaining the full inherited routine,
trigger, event, package, parse-context, runtime-context, and instruction
runtime is dead weight for the current embedded profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `docs/specs/non-table-object-policy/specs.md` already marks views, triggers,
  routines, packages, sequences, and `CALL` as unsupported at the public
  `libmylite` SQL boundary.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_PROCEDURE`,
  `SQLCOM_CREATE_SPFUNCTION`, `SQLCOM_CALL`, trigger DDL, view DDL, package
  DDL, and event DDL through server object paths rather than the MyLite storage
  handler.
- `mariadb/sql/sp.cc` implements routine handler lookup, cache, create/drop,
  package resolution, privilege checks, and `mysql.proc` plumbing.
- `mariadb/sql/sp_head.cc` implements stored-program compilation state,
  execution, parameter binding, table-list merging, optimization, and
  `SHOW CREATE` support.
- `mariadb/sql/sp_instr.cc` implements stored-program instruction execution
  and vtables for statement, jump, handler, cursor, set, return, and error
  instructions.
- `mariadb/sql/sp_pcontext.cc` and `mariadb/sql/sp_rcontext.cc` implement
  stored-program parse and runtime contexts, variables, handlers, cursors, and
  condition handling.
- Before this slice, `mariadb/libmysqld/CMakeLists.txt` included `../sql/sp.cc`,
  `../sql/sp_cache.cc`, `../sql/sp_head.cc`, `../sql/sp_instr.cc`,
  `../sql/sp_pcontext.cc`, and `../sql/sp_rcontext.cc` in
  `SQL_EMBEDDED_SOURCES`.

## Design

- Add `MYLITE_WITH_STORED_PROGRAM_RUNTIME`, defaulting to `ON` for
  upstream-compatible embedded build behavior.
- Set `MYLITE_WITH_STORED_PROGRAM_RUNTIME=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, remove the six full
  `sp*.cc` stored-program runtime objects from `SQL_EMBEDDED_SOURCES` and link
  `mylite_stored_program_runtime_stub.cc`.
- The stub preserves the symbols still referenced by the generated parser,
  retained SQL helpers, trigger/view/table code, and prepared-statement code.
  These symbol bodies must either fail closed with `ER_NOT_SUPPORTED_YET`,
  return conservative empty/no-op values for cleanup/cache paths, or preserve
  parser bookkeeping that cannot succeed without later unsupported diagnostics.
- Keep public `libmylite` SQL-policy rejection for stored-program and
  non-table-object statements. Public callers should not reach the stub during
  normal direct or prepared execution.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- Stored routine, function, trigger, event, and package compiler/runtime roots.
- Stored-program cache and runtime context cleanup.
- Public non-table-object SQL policy tests.
- Compatibility, size-profile, and roadmap documentation.

## MySQL/MariaDB Compatibility Impact

Stored routine, trigger, event, package, compound-statement, and `CALL`
surfaces remain out of scope for the current MyLite embedded profile. Public
behavior is explicit rejection before MariaDB execution. Internal embedded
callers that bypass MyLite SQL policy receive a fail-closed MariaDB diagnostic
instead of linking the full inherited runtime.

The normal MariaDB `sql` target keeps the full stored-program runtime for
comparison builds.

## DDL Metadata Routing Impact

No MyLite routine or trigger metadata format is introduced. Removing the
runtime prevents inherited filesystem or `mysql.proc` publication from becoming
observable before catalog-backed object storage is designed.

## Single-File And Embedded-Lifecycle Impact

This aligns the default embedded profile with the single-file product shape:
stored programs have no durable `.mylite` catalog representation and no file
lifecycle or recovery guarantees yet. The stub must not create durable sidecar
metadata or rely on server-owned system tables.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Existing public rejection for
non-table object SQL remains `MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB errno
`0`, and a stable MyLite diagnostic.

## Storage-Engine Routing Impact

None for supported table DDL/DML. Stored-program object classes do not yet have
a MyLite storage-engine or catalog-backed route.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol integrations over the MyLite core should inherit the same
unsupported policy. A raw MariaDB adapter must add an equivalent gate before it
can be considered a supported entry point.

## Binary-Size Impact

Before this slice, the default archive contained:

- `sp.cc.o`
- `sp_cache.cc.o`
- `sp_head.cc.o`
- `sp_instr.cc.o`
- `sp_pcontext.cc.o`
- `sp_rcontext.cc.o`

The bundle-size research measured this as one of the largest remaining
server-surface trims. Current measurements are recorded in the implementation
results below.

## Implementation Results

The default embedded profile now sets
`MYLITE_WITH_STORED_PROGRAM_RUNTIME=OFF`. The embedded source list removes the
six full stored-program runtime objects and links
`mylite_stored_program_runtime_stub.cc` instead.

The stub fail-closes stored routine, function, trigger, event, package,
stored-program instruction, parse-context, runtime-context, `%TYPE`, and
`%ROWTYPE` paths that are still referenced by retained MariaDB parser, item,
field, cleanup, and trigger/view/table helpers. It also preserves conservative
no-op cleanup/cache paths where the caller is tearing down state rather than
executing unsupported stored-program behavior.

Measured on 2026-05-15:

| Profile | Archive | Size | Members | Delta From Previous Baseline |
| --- | --- | ---: | ---: | ---: |
| Default embedded | `build/mariadb-embedded/libmysqld/libmariadbd.a` | 27,922,072 bytes / 26.63 MiB | 683 | -195,896 bytes, -5 members |
| Storage-smoke | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` | 28,102,648 bytes / 26.80 MiB | 686 | -195,904 bytes, -5 members |

Both archives contain `mylite_stored_program_runtime_stub.cc.o` and no
`sp.cc.o`, `sp_cache.cc.o`, `sp_head.cc.o`, `sp_instr.cc.o`,
`sp_pcontext.cc.o`, or `sp_rcontext.cc.o` members.

## License And Dependency Impact

No new dependency. The replacement stub is GPL-2.0-compatible first-party
MyLite code inside the GPL-2.0 MariaDB-derived tree.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both embedded archives contain
  `mylite_stored_program_runtime_stub.cc.o` and no full stored-program runtime
  objects.
- Confirm retained references to `sp_head`, `sp_instr`, `sp_pcontext`,
  `sp_rcontext`, `%TYPE`, and `%ROWTYPE` helpers resolve to the MyLite stub
  member rather than to the full MariaDB runtime objects.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full stored-program runtime.
- Run dev tests, format, shell syntax, diff, and tidy checks.

Verification commands:

```sh
tools/mariadb-embedded-build configure
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '(^|/)sp\.cc|sp_cache|sp_head|sp_instr|sp_pcontext|sp_rcontext|mylite_stored'
nm -g build/mariadb-embedded/libmysqld/libmariadbd.a | c++filt | rg 'Qualified_column_ident::resolve_type_ref|Table_ident::resolve_table_rowtype_ref'
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure
ar -t build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | rg '(^|/)sp\.cc|sp_cache|sp_head|sp_instr|sp_pcontext|sp_rcontext|mylite_stored'
nm -g build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | c++filt | rg 'Qualified_column_ident::resolve_type_ref|Table_ident::resolve_table_rowtype_ref'
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

## Acceptance Criteria

- The default embedded cache records
  `MYLITE_WITH_STORED_PROGRAM_RUNTIME=OFF`.
- Public direct and prepared non-table object SQL rejection still happens
  before MariaDB execution.
- The embedded archive links the MyLite stored-program runtime stub instead of
  the full `sp*.cc` runtime objects.
- Ordinary MyLite open/close, direct SQL, prepared SQL, warnings, comparison,
  and storage-smoke coverage still pass.
- Documentation and compatibility matrix mark stored-program runtime explicitly
  unsupported for the default embedded profile.
- Size measurements are recorded.

## Risks And Unresolved Questions

- The generated parser still contains stored-program grammar actions. The stub
  must satisfy parser references without enabling partial stored-program
  behavior.
- Several retained SQL paths refer to `sp_head`, `sp_pcontext`, `sp_rcontext`,
  or stored routine handlers for cleanup, diagnostics, `SHOW CREATE`, triggers,
  or prepared `CALL`. Stub behavior must be conservative on each path.
- This is a large symbol-surface replacement. Build/link success is not enough;
  the existing SQL and storage-smoke coverage is required before commit.
