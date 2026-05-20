# Option Help Text Trim

## Problem Statement

The embedded archive still includes MariaDB's hardcoded command-line option
help prose from `my_long_options[]`. This text is useful for server-style
`--help` output, but it is documentation metadata rather than embedded SQL,
native storage, database-directory lifecycle, or public `libmylite` behavior.

This slice removes only the prose comments from the default embedded archive.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/mysqld.cc` defines the server `my_long_options[]` table.
- Each `my_option` entry carries option name, short name, comment, target
  variables, type, argument policy, defaults, bounds, aliases, and block size.
- `mariadb/mysys/my_getopt.c` uses the same table for option parsing and
  `my_print_help()` uses `my_option::comment` for generated help text.
- The MyLite embedded runtime already starts MariaDB with controlled embedded
  arguments and test coverage exercises the resulting startup path.

## Proposed Design

Add `MYLITE_WITH_OPTION_HELP_TEXT`, defaulting to `ON` for normal MariaDB
builds. The MyLite embedded baseline sets it to `OFF`.

When disabled, `mariadb/sql/mysqld.cc` maps only the hardcoded option comment
field through `MYLITE_OPTION_HELP_TEXT(...)`, which expands to an empty string.
The option table still keeps names, aliases, variables, argument types,
defaults, bounds, and parser metadata.

## Affected MariaDB Subsystems

- Embedded CMake profile options and definitions.
- `my_long_options[]` comments in `mysqld.cc`.
- Embedded archive measurement output.

## Compatibility Impact

No SQL, storage-engine, public C API, or database-directory behavior changes.
The only behavior difference is that inherited MariaDB-style command-line help
descriptions are empty in the embedded archive. Option parsing metadata remains
compiled normally.

## Database-Directory And Lifecycle Impact

None. No runtime files, native storage files, locks, or temporary paths change.

## Public API Impact

None. `libmylite` headers, symbols, diagnostics, and SQL entry points are
unchanged.

## Native Storage Impact

None. InnoDB, MyISAM, Aria, MEMORY, and metadata inspection paths are
unchanged.

## Binary-Size Impact

Measured on 2026-05-20 with `tools/mariadb-embedded-build all`, omitting
command-line option help text reduces the stripped embedded archive to
27,128,952 bytes / 25.87 MiB with 705 members. That is 8,680 bytes smaller
than the previous 27,137,632-byte static `SHOW` information baseline, with no
member-count change.

## License Or Dependency Impact

No new dependencies or license changes.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm representative inherited option-help strings are absent from the
  measured archive.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The default embedded baseline sets `MYLITE_WITH_OPTION_HELP_TEXT=OFF`.
- Normal MariaDB builds keep the option defaulted to `ON`.
- Embedded startup option parsing remains covered by the existing embedded test
  suite.
- Representative `my_long_options[]` help strings are absent from the measured
  archive.
- Current archive size and member count are recorded.

## Risks And Unresolved Questions

- This removes inherited `--help` prose from the embedded archive. That is
  acceptable for the core library profile because MyLite's public contract is
  the `libmylite` API and embedded startup configuration, not a daemon command
  interface.
- The option table remains in place because the embedded runtime still uses
  retained MariaDB option parsing for controlled startup arguments.
