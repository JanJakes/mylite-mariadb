# Embedded SQL No-Exceptions Profile

## Problem

The current embedded MariaDB archive still compiles the retained SQL layer with
C++ exception support. Historical size research shows a meaningful linked and
archive reduction from compiling only `sql_embedded` with `-fno-exceptions`,
but the current retained source still includes MariaDB's fmtlib-backed
`SFORMAT()` SQL function, whose implementation catches `fmt::format_error`.

This slice makes `SFORMAT()` an explicit unsupported optional SQL function in
the MyLite embedded profile, then compiles only the embedded SQL archive target
without C++ exceptions. First-party MyLite C++ code and non-embedded MariaDB
targets remain exception-capable.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/item_strfunc.cc:60-61` enables header-only fmtlib support and
  includes `fmt/args.h`.
- `mariadb/sql/item_strfunc.h:799-815` declares `Item_func_sformat`.
- `mariadb/sql/item_strfunc.cc:1502-1646` implements `Item_func_sformat`,
  including `fmt::dynamic_format_arg_store`, `fmt::vformat()`, and a
  `catch (const fmt::format_error &ex)` block.
- `mariadb/sql/item_create.cc:2332-2345` declares the `Create_func_sformat`
  builder.
- `mariadb/sql/item_create.cc:5765-5783` defines the builder singleton and
  constructs `Item_func_sformat`.
- `mariadb/sql/item_create.cc:6594` registers `SFORMAT` in the native function
  registry.
- `mariadb/libmysqld/CMakeLists.txt:214` creates the retained
  `sql_embedded` convenience library used by `libmariadbd.a`.
- `mariadb/sql/CMakeLists.txt:488` and
  `mariadb/libmysqld/CMakeLists.txt:221` add build dependencies on `libfmt`
  when the target exists.
- `mariadb/CMakeLists.txt:436` still checks or configures fmtlib globally; this
  slice can avoid building the dependency when no retained target uses it, but
  it does not redesign MariaDB's top-level fmt discovery.

Official MariaDB references:

- <https://mariadb.com/docs/server/reference/sql-functions/string-functions/sformat>
- <https://mariadb.com/docs/server/reference/sql-functions/string-functions/format>

## Scope

- Add `MYLITE_WITH_SFORMAT_SQL_FUNCTION`, defaulting to `ON`.
- Set `MYLITE_WITH_SFORMAT_SQL_FUNCTION=OFF` in the default MyLite embedded
  profile.
- Guard the `SFORMAT()` builder, registry entry, `Item_func_sformat`
  declaration, implementation, and fmtlib-only include/code when the option is
  off.
- Avoid adding a `libfmt` target dependency to `sql` and `sql_embedded` when
  `SFORMAT()` is disabled.
- Add `MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS`, defaulting to `ON`.
- Set `MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS=OFF` in the default MyLite embedded
  profile.
- When embedded SQL exceptions are off, compile only `sql_embedded` C++ sources
  with `-fno-exceptions`.
- Reject public direct and prepared `SFORMAT()` calls before MariaDB execution
  with stable MyLite diagnostics.

## Non-Goals

- Disable C++ exceptions in first-party MyLite code.
- Disable exceptions in all MariaDB targets.
- Remove ordinary numeric `FORMAT()`, `DATE_FORMAT()`, `TIME_FORMAT()`, or
  `GET_FORMAT()`.
- Remove fmtlib discovery from MariaDB's top-level configure flow.
- Replace `SFORMAT()` with a MyLite formatting implementation.
- Claim that all future embedded SQL sources are exception-free without build
  coverage.

## Design

Use the same profile-option pattern as recent SQL function trims. Define
`MYLITE_WITH_SFORMAT_SQL_FUNCTION` in both `mariadb/sql/CMakeLists.txt` and
`mariadb/libmysqld/CMakeLists.txt`, and expose it to sources as
`MYLITE_WITH_SFORMAT_SQL_FUNCTION`.

When the option is off:

- do not include `fmt/args.h` from `item_strfunc.cc`;
- do not compile `Item_func_sformat` methods;
- do not declare `Item_func_sformat` in `item_strfunc.h`;
- do not declare or define `Create_func_sformat`;
- do not register `SFORMAT` in `native_func_registry_array`;
- do not depend on `libfmt` from `sql` or `sql_embedded`.

Define `MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS` in
`mariadb/libmysqld/CMakeLists.txt`. When it is off, apply `-fno-exceptions` to
the C++ compile language for `sql_embedded` only. Keep C sources and all other
targets on their existing compiler flags.

In `libmylite`, add `SFORMAT()` to the unsupported SQL-surface scanner. This
keeps public diagnostics stable and avoids relying on MariaDB's unknown-function
path after the builder is absent.

## Compatibility Impact

`SFORMAT()` becomes an explicit unsupported SQL function in the default embedded
profile. This is a compatibility tradeoff because MariaDB documents `SFORMAT()`
as a string-formatting function available from MariaDB 10.7.

The tradeoff is acceptable for the current default profile because `SFORMAT()`
is MariaDB-specific, fmtlib-backed, and not part of common MySQL application SQL.
Ordinary MySQL/MariaDB formatting functions such as `FORMAT()`,
`DATE_FORMAT()`, `TIME_FORMAT()`, and `GET_FORMAT()` remain available.

## DDL Metadata Routing Impact

No table metadata, catalog format, generated-column, CHECK, or DDL routing
behavior changes are intended. Generated columns or CHECK constraints that use
`SFORMAT()` should fail through the same public SQL policy before MariaDB
execution; broader expression matrices remain separate coverage.

## Single-File And Embedded-Lifecycle Impact

No durable file, sidecar, startup, shutdown, lock, recovery, or cleanup behavior
changes. Avoiding an embedded `libfmt` target dependency may reduce build-time
third-party work when the host has no system fmtlib, but it does not change the
runtime file lifecycle.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes. Public SQL execution and
prepare APIs return stable MyLite diagnostics for `SFORMAT()` calls.

## Storage-Engine Routing Impact

No routed engine behavior changes.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. A future compatibility profile can keep
`MYLITE_WITH_SFORMAT_SQL_FUNCTION=ON` and
`MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS=ON` if it needs exact MariaDB `SFORMAT()`
behavior.

## Binary-Size Impact

`docs/architecture/bundle-size-research.md` records prior evidence for this
family of changes:

- omitting the fmtlib-backed `SFORMAT()` SQL function saved 72,280 bytes from a
  stripped linked runtime proxy and 291,688 bytes from the embedded archive;
- compiling only retained `sql_embedded` sources with `-fno-exceptions` after
  removing retained exception users saved 458,256 bytes from a stripped linked
  runtime proxy and 2,554,764 bytes from the embedded archive.

This slice remeasured the current branch because the default embedded profile
had already trimmed LOAD execution, SQL host-file I/O, server utility functions,
Oracle SQL mode parsing, XML SQL functions, and GIS SQL functions.

Current measurements on 2026-05-15, after this slice:

- default embedded archive: 28,253,816 bytes / 26.94 MiB, 689 members;
- storage-smoke archive: 28,434,400 bytes / 27.12 MiB, 692 members;
- both archives are 2,000,008 bytes smaller than the previous GIS-trimmed
  baseline with unchanged archive member counts;
- linked smoke binaries now strip to 15.52-15.82 MiB, down by roughly
  367-384 KiB per binary from the previous GIS-trimmed baseline;
- linked smoke global symbol counts drop by 19, with comparison smokes at
  16,240 and other smokes at 16,238.

## License Or Dependency Impact

No new dependencies and no license change. Disabling `SFORMAT()` reduces the
default embedded profile's need to build fmtlib, which MariaDB treats as an MIT
third-party dependency when bundled.

## Test And Verification Plan

- Add direct execution policy tests for `SFORMAT()`.
- Add prepared-statement policy coverage for `SFORMAT()`.
- Verify quoted mentions of `SFORMAT(` still execute as ordinary strings.
- Verify ordinary `FORMAT()` still executes.
- Reconfigure and rebuild the default embedded MariaDB archive and the
  storage-smoke MariaDB archive.
- Verify `sql_embedded` C++ compile commands use `-fno-exceptions`.
- Verify embedded archives no longer depend on the `SFORMAT()` implementation
  path and still build/link all first-party smoke binaries.
- Build and run embedded, storage-smoke, and dev presets.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, normal MariaDB `sql` target, and
  first-party tidy checks.

## Acceptance Criteria

- The default MyLite embedded profile records
  `MYLITE_WITH_SFORMAT_SQL_FUNCTION=OFF` and
  `MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS=OFF`.
- `sql_embedded` C++ compilation uses `-fno-exceptions`.
- Public direct and prepared entry points reject `SFORMAT()` with stable MyLite
  diagnostics.
- Ordinary `FORMAT()` remains available.
- Size documentation records the current measured impact.

## Risks And Open Questions

- `SFORMAT()` is documented MariaDB behavior, so applications that use it will
  need a future compatibility profile or application-side formatting.
- `-fno-exceptions` is intentionally limited to `sql_embedded`. Expanding it to
  first-party code or all MariaDB targets would be a separate, higher-risk
  decision.
- MariaDB's top-level fmt discovery remains in configure. This slice only
  removes the build dependency from targets that no longer use fmtlib.
