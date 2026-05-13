# Option Help Text Size Profile

## Problem

After `sysvar-help-text-size-profile`, the aggressive MyLite minsize profile
still retains hardcoded `my_long_options[]` help strings from
`sql/mysqld.cc`. These strings describe daemon command-line behavior such as
replication, binlog, plugin loading, logging, and service startup. MyLite still
needs option names and parsing for embedded bootstrap arguments, but it does
not need verbose `mariadbd --help` prose in the linked embedded library.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/mysqld.cc` defines `my_long_options[]`, a
  `my_option` array used by `handle_options()` through `get_options()`.
- `my_long_options[]` remains linked in the current smoke binary as a
  7,616-byte data symbol, and its comment string literals remain visible in
  `.rodata`.
- `vendor/mariadb/server/include/my_getopt.h` documents
  `my_option::comment` as the automated `--help` text; a `NULL` comment hides
  the option from help.
- `vendor/mariadb/server/mysys/my_getopt.c` uses `comment` only in
  `my_print_help()`. Option parsing uses option names, ids, value pointers,
  types, defaults, ranges, and callback handling.
- `mysqld.cc::usage()` and `print_help()` are compiled out under
  `EMBEDDED_LIBRARY`, so the embedded runtime does not expose the normal
  `mariadbd --verbose --help` flow.
- `mysqld.cc` also defines `pfs_early_options[]`, but the current linked smoke
  binary does not retain its help strings in the minsize profile.

## Scope

This slice may:

- add `MYLITE_DISABLE_OPTION_HELP_TEXT`,
- enable it from `tools/build-mariadb-minsize.sh`,
- wrap `my_long_options[]` comment fields so they become empty strings in the
  aggressive embedded profile,
- keep option names, ids, values, defaults, ranges, deprecation substitutes,
  and parsing callbacks intact,
- keep system-variable help text handling from the previous slice unchanged,
  and
- record measured size deltas.

## Non-Goals

This slice does not:

- remove `my_long_options[]` entries,
- change MyLite bootstrap arguments,
- change system-variable comments,
- change option names, default values, validation ranges, or aliases,
- promise `mariadbd --help` compatibility in the aggressive embedded profile,
  or
- change non-minsize builds.

## Proposed Design

Add a minsize-only CMake option and a local `MYLITE_OPTION_HELP_TEXT(...)`
macro near `my_long_options[]`. Under the aggressive embedded profile, the
macro expands every wrapped comment field to `""`; otherwise it preserves the
original upstream help text.

The macro must be used at the initializer site, not in a later copy path, so
the preprocessor discards the long string tokens before compilation. This
mirrors the declaration-site lesson from the system-variable help text slice.

## Affected Subsystems

- Minsize CMake configuration.
- Server option table initialization in `mysqld.cc`.
- MyLite open/close smoke coverage.
- Production size analysis.

## Single-File and Embedded Lifecycle Impact

No file-format or storage lifecycle change. Startup option parsing remains
intact because names and option metadata are preserved.

## Public API and File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

Compatibility impact is limited to the aggressive embedded profile's inherited
server help metadata. MyLite does not expose `mariadbd --help` as a product
surface.

## Binary-Size Impact

Measured on top of `sysvar-help-text-size-profile`:

- `libmariadbd.a`: 27,364,504 -> 27,357,408 bytes (-7,096).
- stripped `mylite-open-close-smoke`: 5,191,128 -> 5,183,976 bytes
  (-7,152).
- stripped `mylite-compatibility-smoke`: 5,065,160 -> 5,058,136 bytes
  (-7,024).
- `lib_sql.cc.o`: 475,880 -> 468,784 bytes (-7,096).

The `my_long_options` data table itself remains because option names and
parsing metadata are retained.

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no new
dependency and changes no trademark-facing packaging.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-option-help-text MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-option-help-text tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-option-help-text tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-option-help-text tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-option-help-text tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
- `bash -n tools/run-libmylite-open-close-smoke.sh`
- `git diff --check`

## Acceptance Criteria

- MyLite open/close, storage, bootstrap, and compatibility smokes pass.
- Existing embedded startup options still parse through the smoke suite.
- Long option help strings such as `Log update queries in binary format` no
  longer appear in the linked open-close smoke binary.
- Measured size changes are recorded in
  `docs/research/production-size-analysis.md`.

## Risks and Unresolved Questions

- If future packaging exposes MariaDB's help output from the embedded library,
  this profile will make those comments empty.
- Removing entire `my_long_options[]` entries may save more, but it is a
  separate, higher-risk startup/default-initialization slice because the table
  feeds `handle_options()`.
