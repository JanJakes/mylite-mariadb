# Optimizer Trace Trim

## Problem Statement

The default embedded archive still builds MariaDB's optimizer trace runtime.
Optimizer trace stores per-session JSON diagnostics for optimizer decisions and
exposes them through `INFORMATION_SCHEMA.OPTIMIZER_TRACE`. That is useful
server debugging data, but it is not durable application behavior, native
storage behavior, or part of MyLite's directory-owned C API contract.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/opt_trace.cc` implements optimizer trace lifecycle, JSON helper
  symbols, trace security checks, `Show::optimizer_trace_info`, and
  `fill_optimizer_trace_info()`.
- `mariadb/sql/sys_vars.cc` registers `optimizer_trace` and
  `optimizer_trace_max_mem_size`.
- `mariadb/sql/sql_show.cc` registers
  `INFORMATION_SCHEMA.OPTIMIZER_TRACE`.
- Retained optimizer, parser, prepared-statement, and stored-program paths
  include `opt_trace.h` and instantiate trace helper wrappers. The safe cut is
  therefore an inert replacement object that preserves symbols, not broad
  source deletion.

## Design

Add `MYLITE_WITH_OPTIMIZER_TRACE`, defaulting to `ON` for upstream-style builds
and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.

When disabled, `mariadb/libmysqld/CMakeLists.txt` replaces
`../sql/opt_trace.cc` with `../sql/mylite_opt_trace_disabled.cc`. The stub
keeps required schema-table field metadata and helper symbols, but never starts,
stores, fills, or exposes optimizer trace rows.

The public MyLite SQL policy rejects:

- assignments to `optimizer_trace`,
- assignments to `optimizer_trace_max_mem_size`,
- references to `INFORMATION_SCHEMA.OPTIMIZER_TRACE`, including unqualified
  reads while `information_schema` is the current schema.

Ordinary SQL planning and execution remain unchanged. `EXPLAIN` remains
available; this slice removes trace diagnostics, not query plans.

## Compatibility Impact

Optimizer trace becomes explicitly unsupported in the default embedded profile.
This removes server diagnostic JSON for optimizer internals, not SQL syntax,
query execution, DDL, DML, JSON SQL functions, GEOMETRY/GIS, native storage
engines, or the public C API.

## Database-Directory And Lifecycle Impact

None. The slice does not add or move durable files, temporary files, locks,
logs, metadata, or runtime directories.

## Public API Impact

None. `libmylite` headers and symbols are unchanged. Direct execution and
prepared statements return stable MyLite unsupported-surface diagnostics for
optimizer-trace SQL.

## Native Storage Impact

None. Native engine routing, table files, transaction behavior, and recovery
behavior are unchanged.

## Binary-Size Impact

`tools/mariadb-embedded-build all` measured the stripped default archive at
27,116,808 bytes / 25.86 MiB with 705 members. This is 12,144 bytes smaller
than the previous 27,128,952-byte baseline, with the same member count.
`opt_trace.cc.o` is absent and `mylite_opt_trace_disabled.cc.o` is present.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `opt_trace.cc.o` is absent and `mylite_opt_trace_disabled.cc.o` is
  present in `libmariadbd.a`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The embedded baseline configures `MYLITE_WITH_OPTIMIZER_TRACE=OFF`.
- The embedded archive links `mylite_opt_trace_disabled.cc.o` instead of
  `opt_trace.cc.o`.
- Direct and prepared optimizer-trace SQL is rejected through MyLite policy,
  including the qualified and current-schema information-schema table paths.
- Ordinary planning, execution, and `EXPLAIN` remain available.
- Architecture, compatibility, API, roadmap, and size-profile docs describe the
  unsupported diagnostic surface and measured size impact.

## Risks And Unresolved Questions

- Trace helper wrappers are instantiated throughout retained optimizer paths.
  The disabled object must keep those symbols inert rather than deleting the
  header-level API.
- MariaDB still registers optimizer-trace system variables and
  `INFORMATION_SCHEMA.OPTIMIZER_TRACE` metadata. MyLite policy rejects writes,
  qualified table reads, and current-schema table reads before users can
  observe a misleading no-op diagnostic surface.
