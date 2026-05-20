# System Variable Help Text Trim

## Problem Statement

The embedded archive still retains long MariaDB system-variable help strings.
Those strings describe command-line/server behavior for `mariadbd --help` and
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT`. MyLite still needs
system-variable names, values, defaults, scopes, validation, and `SHOW
VARIABLES`, but the descriptive prose is not important runtime functionality in
the core embedded profile.

This slice removes those long comments from the default embedded archive while
preserving the variables themselves.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sys_vars.cc` declares retained system variables and passes
  human-readable comments into `Sys_var_*` constructors.
- `mariadb/sql/sys_vars.inl` forwards those comments through inline
  constructors into the base `sys_var` object.
- `mariadb/sql/set_var.cc` stores the comment pointer in
  `sys_var::option.comment`.
- `fill_sysvars()` uses `option.comment` for the
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` column.
- `SHOW VARIABLES`, `INFORMATION_SCHEMA.GLOBAL_VARIABLES`, and
  `INFORMATION_SCHEMA.SESSION_VARIABLES` use names and value pointers, not the
  long comments.
- Historical size research found that constructor-only empty comments still
  left original string literals rooted by static constructor call sites. The
  declaration-site comment arguments must be wrapped so the preprocessor can
  discard the literals before compilation.

## Proposed Design

Add `MYLITE_WITH_SYSVAR_HELP_TEXT`, defaulting to `ON` for normal MariaDB
builds. The MyLite embedded baseline sets it to `OFF`.

When disabled, `mariadb/libmysqld/CMakeLists.txt` defines
`MYLITE_DISABLE_SYSVAR_HELP_TEXT`. `mariadb/sql/sys_vars.inl` maps
`MYLITE_SYSVAR_HELP_TEXT(...)` to an empty string under that macro and to the
original text otherwise. `mariadb/sql/sys_vars.cc` wraps system-variable
comment arguments in `MYLITE_SYSVAR_HELP_TEXT(...)` so long literals disappear
from the embedded compilation unit.

`fill_sysvars()` remains unchanged. It receives a valid empty C string for
`VARIABLE_COMMENT`.

## Affected MariaDB Subsystems

- Embedded CMake profile options and definitions.
- System-variable declaration text in `sys_vars.cc`.
- `Sys_var_*` constructor forwarding in `sys_vars.inl`.
- `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT`.

## Compatibility Impact

This does not remove system variables, change defaults, change validation, or
change `SHOW VARIABLES`. The only SQL-visible difference is that
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default
embedded profile.

That column is help text, not application data semantics. The compatibility
matrix records the embedded-profile behavior.

## Database-Directory And Lifecycle Impact

None. No runtime files, native storage files, locks, or temporary paths change.

## Public API Impact

None. `libmylite` headers and symbols are unchanged.

## Native Storage Impact

None. Native storage engines and their variables remain present according to
the existing profile.

## Binary-Size Impact

Measured on 2026-05-20 with `tools/mariadb-embedded-build all`, omitting
system-variable help text reduces the stripped embedded archive to 27,170,568
bytes / 25.91 MiB with 705 members. That is 56,040 bytes smaller than the
previous 27,226,608-byte `PROCEDURE ANALYSE()` baseline, with no member-count
change because the slice removes read-only string data from existing objects.

## License Or Dependency Impact

No new dependencies or license changes.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
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

- The default embedded baseline sets `MYLITE_WITH_SYSVAR_HELP_TEXT=OFF`.
- Normal MariaDB builds keep the option defaulted to `ON`.
- System-variable values and `SHOW VARIABLES` remain available.
- `INFORMATION_SCHEMA.SYSTEM_VARIABLES` still exposes variable rows.
- `VARIABLE_COMMENT` is empty for covered rows in the default embedded profile.
- Current archive size and member count are recorded.

## Risks And Unresolved Questions

- Clients that render help UIs from `INFORMATION_SCHEMA.SYSTEM_VARIABLES` lose
  descriptive comments in the embedded profile. Variable names, values, and
  metadata remain available.
- Other hardcoded command-line option help strings remain. Those should be a
  separate size slice because they live outside system-variable comments.
