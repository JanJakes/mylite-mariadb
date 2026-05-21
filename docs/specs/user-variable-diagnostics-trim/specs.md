# User Variable Diagnostics Trim

## Problem Statement

MariaDB's `user_variables` plugin exposes optional diagnostics for session
user variables through `INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW
USER_VARIABLES`, and `FLUSH USER_VARIABLES`. MyLite should keep ordinary
`@variable` SQL behavior, but the diagnostic plugin is not necessary for the
embedded default profile.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/plugin/user_variables/CMakeLists.txt` registers the
  `user_variables` Information Schema plugin as a default embedded plugin.
- `mariadb/plugin/user_variables/user_variables.cc` only defines the
  Information Schema table fields, fill hook, and reset hook for
  `USER_VARIABLES`.
- MariaDB core user-variable execution and session tracking stay in retained
  SQL sources such as `mariadb/sql/item_func.cc`,
  `mariadb/sql/session_tracker.cc`, and `mariadb/sql/sql_class.cc`.

## Proposed Design

Set `PLUGIN_USER_VARIABLES=NO` in the MyLite embedded baseline so the static
diagnostic plugin is not linked into `libmariadbd.a`.

MyLite policy rejects direct and prepared access to
`INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW USER_VARIABLES`, and
`FLUSH USER_VARIABLES`. Ordinary local variables such as `@value` and ordinary
application tables named `user_variables` remain valid outside
`information_schema`.

## Compatibility Impact

No supported application-data behavior is removed. Local `@variable` SQL
assignment and reads remain available. The omitted surface is a server
introspection/reset helper for session variables.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,484,960 bytes / 25.26 MiB with 701 members, down 6,576 bytes and one
archive member from the prior 26,491,536-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `PLUGIN_USER_VARIABLES=NO` appears in the embedded CMake cache.
- Confirm `user_variables.cc.o` is absent from `libmariadbd.a`.
- Verify local `@variable` assignment and reads still work.
- Verify direct and prepared user-variable diagnostic SQL fails through the
  MyLite server-surface policy.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The embedded archive omits the `user_variables` plugin.
- Local user variables remain usable through ordinary SQL.
- User-variable diagnostic tables, `SHOW USER_VARIABLES`, and reset statements
  fail explicitly through the MyLite policy.
- Ordinary application tables named `user_variables` remain usable outside
  `information_schema`.
