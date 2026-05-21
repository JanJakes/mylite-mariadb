# Userstat Diagnostics Trim

## Problem Statement

MariaDB's `userstat` plugin exposes optional server diagnostic tables:
`INFORMATION_SCHEMA.CLIENT_STATISTICS`, `INDEX_STATISTICS`,
`TABLE_STATISTICS`, and `USER_STATISTICS`. MyLite's embedded profile should
keep application SQL, native storage, JSON, GEOMETRY, and core diagnostics, but
can omit these inherited server counters safely.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/plugin/userstat/CMakeLists.txt` registers `USERSTAT` as a mandatory
  static plugin.
- `mariadb/plugin/userstat/userstat.cc` declares four Information Schema
  plugins for `CLIENT_STATISTICS`, `INDEX_STATISTICS`, `TABLE_STATISTICS`, and
  `USER_STATISTICS`.
- `mariadb/sql/sys_vars.cc` registers the `userstat` system variable, which
  enables collection for those diagnostic tables.
- The core statistics data structures and native table statistics code live in
  retained SQL sources such as `sql_connect.cc`, `handler.cc`, and
  `sql_statistics.cc`; this slice does not remove those shared paths.

## Proposed Design

Add `MYLITE_WITH_USERSTAT_DIAGNOSTICS`, defaulting to `ON` for normal MariaDB
builds and forced `OFF` in the MyLite embedded baseline. When disabled, CMake
does not register the static `USERSTAT` plugin, and `sys_vars.cc` omits the
`userstat` system-variable row.

MyLite policy rejects direct and prepared access to the userstat Information
Schema tables, `FLUSH *_STATISTICS`, and attempts to set `userstat`. Ordinary
application tables with the same unqualified names outside
`information_schema` remain valid.

## Compatibility Impact

No supported application-data behavior is removed. Userstat diagnostics are
server counters, not core SQL execution, storage, public API diagnostics, JSON,
GEOMETRY, or native engine behavior.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,491,536 bytes / 25.26 MiB with 702 members, down 19,528 bytes and one
archive member from the prior 26,511,064-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_USERSTAT_DIAGNOSTICS=OFF` appears in the embedded CMake
  cache.
- Confirm `userstat.cc.o` and the included userstat table sources are absent
  from `libmariadbd.a`.
- Verify direct and prepared `SELECT @@userstat` fail with MariaDB unknown
  system-variable errno.
- Verify direct and prepared userstat diagnostic SQL fails through the MyLite
  server-surface policy.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The embedded archive omits the `userstat` plugin.
- The embedded profile does not expose the `userstat` system variable.
- Userstat diagnostic tables and reset statements fail explicitly through the
  MyLite policy.
- Ordinary application tables named like the diagnostic tables remain usable
  outside `information_schema`.
