# Server Surface Profile Trim

## Goal

Compile server-oriented plugin surfaces out of the default MariaDB embedded
profile once runtime policy has made them unsupported and testable. The trim
should reduce the embedded archive without removing application-visible SQL
features such as sequences or user variables.

## Non-Goals

- Do not remove charsets, collations, SQL functions, or parser sources.
- Do not remove MariaDB sequence support or user-variable support in this slice.
- Do not remove mandatory engines used by MariaDB internals or current MyLite
  smoke coverage.
- Do not replace dynamic-plugin SQL policy gates; rejected SQL should remain
  stable even after build support is compiled out.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/cmake/plugin.cmake` honors `PLUGIN_<name>=NO` for non-mandatory
  plugins and excludes them from `EMBEDDED_PLUGIN_LIBS` and
  `MYSQLD_STATIC_PLUGIN_LIBS`.
- `mariadb/storage/perfschema/CMakeLists.txt` registers `perfschema` as a
  default static embedded storage engine. When compiled in, MyLite currently
  starts it disabled with `--performance-schema=OFF`.
- `mariadb/plugin/feedback/CMakeLists.txt` registers the feedback plugin as a
  default embedded plugin and links OpenSSL libraries. `mariadb/sql/sql_plugin.cc`
  marks feedback off in the builtin plugin policy.
- `mariadb/plugin/auth_socket/CMakeLists.txt` registers the socket authentication
  plugin by default when platform credential APIs are available. MyLite starts
  with `--skip-grant-tables` and rejects account-management SQL.
- `mariadb/sql/CMakeLists.txt` registers `thread_pool_info` as a default
  server-only static plugin. It is not embedded, but disabling it keeps the
  configured server plugin profile aligned with MyLite's no-daemon runtime.
- `WITHOUT_DYNAMIC_PLUGINS=ON` disables dynamic plugin module support at
  configure time. MyLite already uses an empty runtime plugin directory and
  rejects representative `INSTALL PLUGIN` / `INSTALL SONAME` SQL.
- Probe build evidence on 2026-05-15:
  - default embedded archive: 33,736,928 bytes / 32.17 MiB, 807 members;
  - trimmed embedded archive: 32,048,256 bytes / 30.56 MiB, 691 members;
  - trimmed storage-smoke archive with `PLUGIN_MYLITE_SE=STATIC`:
    32,162,624 bytes / 30.67 MiB, 693 members.

## Compatibility Impact

The removed surfaces are out of scope for the core embedded product:

- Performance Schema instrumentation tables are now either runtime-disabled or
  compiled out.
- Dynamic plugin installation remains rejected by MyLite SQL policy.
- Server socket authentication and grant-backed account management remain
  outside the local file-owned runtime.
- Feedback and thread-pool-info plugins are daemon/server-administration
  surfaces.

The slice deliberately keeps MariaDB sequence and user-variable plugins enabled
because they are closer to application-visible SQL compatibility and need
separate coverage before any removal.

`docs/COMPATIBILITY.md` describes Performance Schema as disabled or compiled
out.

## Design

Update `cmake/mariadb-embedded-baseline.cmake` to set:

- `WITHOUT_DYNAMIC_PLUGINS=ON`,
- `PLUGIN_PERFSCHEMA=NO`,
- `PLUGIN_FEEDBACK=NO`,
- `PLUGIN_AUTH_SOCKET=NO`,
- `PLUGIN_THREAD_POOL_INFO=NO`.

Teach first-party CMake integration to inspect `PLUGIN_PERFSCHEMA` in the
MariaDB build cache and define `MYLITE_MARIADB_HAS_PERFSCHEMA` for `libmylite`.
When Performance Schema is absent, `libmylite` must not pass the unsupported
`--performance-schema=OFF` startup option. The server-surface test should accept
either an `OFF` variable or a missing variable.

Update size measurement output and architecture docs so future trims compare
the new default profile.

## File Lifecycle

No `.mylite` file-format or companion-file behavior changes. Disabling dynamic
plugins reduces host plugin exposure; MyLite still creates an empty runtime
plugin directory for inherited MariaDB paths.

## Embedded Lifecycle And API

`mylite_open()` and `mylite_close()` behavior is unchanged. Startup arguments
become conditional on the embedded archive's compiled surface.

## Build, Size, And Dependencies

The default embedded archive should shrink by about 1.61 MiB on the current
macOS arm64 Release build. Dynamic plugin module targets are disabled at
configure time; no new dependency is added.

## Test Plan

1. Build and measure the default embedded archive with the trimmed profile.
2. Build and measure the storage-smoke archive with `PLUGIN_MYLITE_SE=STATIC`.
3. Build and test `embedded-dev` and `storage-smoke-dev`.
4. Run `tools/mylite-size-report` and record updated linked-artifact sizes.
5. Run `tools/mylite-compat-harness run server-surface`.
6. Run format, tidy, diff, and archive-size checks.

## Acceptance Criteria

- The default embedded and storage-smoke archives build with the trimmed profile.
- Embedded startup succeeds whether Performance Schema is compiled in or out.
- Server-surface tests continue to prove networking, binlog, Performance Schema
  state, dynamic plugin SQL, account SQL, event SQL, and replication SQL are not
  enabled.
- Current routed storage smoke passes with the trimmed storage-smoke archive.
- Docs record the measured size impact and compatibility boundary.

## Risks And Open Questions

- `WITHOUT_DYNAMIC_PLUGINS=ON` does not remove inherited dynamic-loading helper
  code that is still referenced from core MariaDB sources. Source-level stubbing
  is a separate slice.
- Removing Performance Schema can make `SHOW VARIABLES LIKE 'performance_schema'`
  return no row instead of `OFF`. The public policy is that the surface is not
  enabled; callers should not depend on the server instrumentation table set in
  the MyLite core profile.
- Sequence and user-variable plugins remain enabled for compatibility until
  separate tests justify a narrower profile.
