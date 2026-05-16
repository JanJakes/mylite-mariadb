# Userstat Plugin Trim

## Goal

Remove MariaDB's user-statistics information-schema plugin from the default
MyLite embedded profile and make the resulting unsupported SQL surface explicit
through public API policy tests.

## Non-Goals

- Do not remove ordinary SQL user variables such as `@name`,
  `SELECT ... INTO @name`, or `SET @name=...`.
- Do not remove the `INFORMATION_SCHEMA.USER_VARIABLES` plugin in this slice.
- Do not remove the core MariaDB status counters or handler accounting fields
  that retained SQL code still updates when the `userstat` system variable is
  enabled.
- Do not add MyLite table or index usage statistics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/plugin/userstat/CMakeLists.txt` registers `USERSTAT` as a
  mandatory plugin with `MYSQL_ADD_PLUGIN(USERSTAT userstat.cc MANDATORY)`.
- `mariadb/plugin/userstat/userstat.cc` declares four
  `MYSQL_INFORMATION_SCHEMA_PLUGIN` entries:
  `CLIENT_STATISTICS`, `INDEX_STATISTICS`, `TABLE_STATISTICS`, and
  `USER_STATISTICS`.
- `mariadb/sql/sys_vars.cc` defines the global `userstat` system variable that
  enables statistics gathering for those information-schema tables.
- `mariadb/sql/sql_parse.cc`, `mariadb/sql/handler.cc`, and
  `mariadb/sql/sql_connect.cc` keep the runtime accounting roots in retained
  server code. This slice only removes the information-schema plugin entry
  points, not those shared counters.
- `mariadb/cmake/plugin.cmake` honors `PLUGIN_<name>=NO` for non-mandatory
  plugins, but mandatory plugins override the cache value, so the userstat
  CMake file needs a narrow MyLite guard before calling `MYSQL_ADD_PLUGIN`.
- MariaDB documentation describes User Statistics as a plugin that creates the
  four information-schema statistics tables:
  <https://mariadb.com/kb/en/user-statistics/>.
- MariaDB documentation also documents `SHOW USER_STATISTICS` and notes that
  the `userstat` system variable activates the feature:
  <https://mariadb.com/kb/en/show-user-statistics/>.

## Compatibility Impact

User-statistics tables and related `SHOW` / `FLUSH` surfaces are
server-observability features, not core embedded MySQL/MariaDB application
behavior. They are out of scope for the current file-owned MyLite runtime.

Ordinary user variables remain in scope and must keep working. This distinction
matters because MariaDB also ships a separate `user_variables` information
schema plugin, and MyLite already documents `SELECT ... INTO` user variables as
available.

## Design

- Add a narrow guard around `MYSQL_ADD_PLUGIN(USERSTAT ...)` so normal MariaDB
  builds keep the mandatory plugin while MyLite can force `PLUGIN_USERSTAT=NO`
  in the embedded baseline.
- Force `PLUGIN_USERSTAT=NO` from
  `cmake/mariadb-embedded-baseline.cmake`.
- Report `PLUGIN_USERSTAT` in `tools/mariadb-embedded-build`.
- Add public SQL policy rejection for:
  - `SHOW CLIENT_STATISTICS`, `SHOW INDEX_STATISTICS`,
    `SHOW TABLE_STATISTICS`, and `SHOW USER_STATISTICS`;
  - `FLUSH` variants for those four statistics table names;
  - `userstat` system-variable assignments, including
    `SET userstat=...`, `SET GLOBAL userstat=...`, and
    `SET @@global.userstat=...`;
  - direct unquoted or backtick-quoted references to
    `INFORMATION_SCHEMA.<userstat table>`.
- Keep ordinary `SET @userstat=...` and quoted mentions of the statistics table
  names available.

## File Lifecycle

No `.mylite` file-format or companion-file change is required. Removing the
information-schema plugin avoids exposing server-owned statistics tables as
part of the embedded file-owned product surface.

## Embedded Lifecycle And API

The public C API continues to return `MYLITE_ERROR` with a stable MyLite
diagnostic for unsupported server-observability SQL before MariaDB execution.
Open, close, repeated startup, and runtime directory ownership are unchanged.

## Build, Size, And Dependencies

The disabled profile omits `userstat.cc.o` from both the default embedded
archive and the opt-in storage-smoke archive. No dependency or license changes
are introduced.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,160,192 bytes / 25.90 MiB | 671 | -19,848 bytes, -1 member |
| Storage-smoke | 27,340,776 bytes / 26.07 MiB | 674 | -19,848 bytes, -1 member |

## Test Plan

1. Reconfigure and rebuild the default MariaDB embedded archive.
2. Reconfigure and rebuild the storage-smoke archive with
   `PLUGIN_MYLITE_SE=STATIC`.
3. Confirm both archives omit `userstat.cc.o` and keep
   `user_variables.cc.o`.
4. Add direct and prepared SQL policy tests for user-statistics `SHOW`, `FLUSH`,
   `userstat` system-variable assignment, and `INFORMATION_SCHEMA` table
   access.
5. Run affected embedded and storage-smoke CTest presets, the server-surface
   compatibility report, the size report, format, tidy, shell syntax, and diff
   checks.

## Acceptance Criteria

- `PLUGIN_USERSTAT=NO` is part of the committed embedded baseline.
- Normal MariaDB builds keep the userstat plugin unless explicitly disabled.
- The default and storage-smoke archives omit `userstat.cc.o` while retaining
  `user_variables.cc.o`.
- Public direct and prepared SQL reject user-statistics server-observability
  surfaces before MariaDB execution.
- Ordinary SQL user variables remain available.
- Compatibility, architecture, roadmap, and size-profile docs describe the
  unsupported boundary and measured size impact.
- Relevant tests and static checks pass.

## Risks And Open Questions

- Some monitoring tools probe `INFORMATION_SCHEMA.TABLE_STATISTICS` or
  `SHOW USER_STATISTICS`. Those tools are server-observability integrations and
  need a future MyLite-native statistics design before compatibility can be
  claimed.
- The retained `userstat` system variable and accounting roots may still be
  removable in a deeper source trim, but that is riskier than removing the
  plugin because the counters are woven through connection, handler, and status
  code.
