# SQL File I/O Size Trim

## Problem

MyLite rejects host-file SQL I/O at the public SQL policy boundary:

- `LOAD_FILE()`,
- `SELECT ... INTO OUTFILE`,
- `SELECT ... INTO DUMPFILE`.

Keeping MariaDB's host-file reader and select-to-file writer bodies in the
default embedded profile preserves code that supported MyLite entry points
should never reach. This slice compiles those bodies out of the MyLite embedded
profile while keeping upstream-style builds unchanged by default.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/item_create.cc:1675-1688` defines the native
  `LOAD_FILE()` builder.
- `mariadb/sql/item_create.cc:4980-4989` creates `Item_load_file`.
- `mariadb/sql/item_strfunc.h:1959-1985` declares `Item_load_file`.
- `mariadb/sql/item_strfunc.cc:4387-4457` reads host files for
  `Item_load_file::val_str()`.
- `mariadb/sql/sql_yacc.yy:13358-13377` creates `select_export` and
  `select_dump` for `INTO OUTFILE` and `INTO DUMPFILE`.
- `mariadb/sql/sql_class.h:6740-6777` declares the select-to-file result
  interceptors.
- `mariadb/sql/sql_class.cc:3420-3456` contains the shared host-file creation
  helper for select-to-file output.
- `mariadb/sql/sql_class.cc:3464-3615` implements text OUTFILE output.
- `mariadb/sql/sql_class.cc:3814-3851` implements binary DUMPFILE output.
- `docs/architecture/bundle-size-research.md` records prior successful trims
  for `LOAD_FILE()` utility-function support and for `SELECT ... INTO OUTFILE`
  / `DUMPFILE` writer bodies.

## Scope

- Add a `MYLITE_WITH_SQL_FILE_IO` CMake option that defaults to `ON`.
- Set `MYLITE_WITH_SQL_FILE_IO=OFF` in the MyLite embedded profile.
- Compile fail-closed stubs for `Item_load_file::val_str()`,
  `select_export::prepare()`, `select_export::send_data()`,
  `select_dump::prepare()`, and `select_dump::send_data()` when the option is
  off.
- Keep parser grammar, native function registration, and class declarations so
  retained MariaDB code links.
- Record the option in embedded build measurement output.

## Non-Goals

- Remove SQL grammar for file-I/O constructs.
- Remove `LOAD_FILE()` from the native function table.
- Remove select-to-file classes from headers.
- Change public MyLite diagnostics; direct and prepared public entry points
  still reject file-I/O SQL before MariaDB execution.
- Trim unrelated utility functions such as `SLEEP()`, `UUID_SHORT()`, or wait
  helpers.

## Design

Define `MYLITE_WITH_SQL_FILE_IO` in both MariaDB source directories that build
SQL objects:

- `mariadb/sql/CMakeLists.txt` for the normal `sql` target;
- `mariadb/libmysqld/CMakeLists.txt` for the embedded `sql_embedded` archive.

When enabled, MariaDB code is unchanged. When disabled, retain the same public
methods but replace host-file I/O bodies with small unsupported stubs. This
keeps the fork delta local to the existing upstream-derived files and avoids
source-list surgery for large shared objects such as `sql_class.cc` and
`item_strfunc.cc`.

## Compatibility Impact

No supported MyLite behavior changes. Public `mylite_exec()` and
`mylite_prepare()` continue to reject file-I/O SQL at the MyLite policy gate.
If internal or future adapter code bypasses that gate, the retained methods
fail closed instead of touching host files.

## Single-File And Embedded-Lifecycle Impact

The trim removes unreachable host-file read/write bodies from the default
embedded profile and does not add durable files or companions.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

None. File-I/O SQL remains outside supported handler paths.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. A future adapter that wants wire-compatible file
import/export must provide a controlled adapter-level implementation.

## Binary-Size Impact

Measured on 2026-05-15 on macOS arm64 with Apple clang 21.0.0:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Baseline embedded archive | 32,024,568 bytes | 32,017,640 bytes | -6,928 bytes |
| Storage-smoke embedded archive | 32,167,392 bytes | 32,160,464 bytes | -6,928 bytes |
| Stripped embedded open-close smoke | 17,642,352 bytes | 17,642,304 bytes | -48 bytes |
| Stripped storage-engine smoke | 17,940,544 bytes | 17,940,496 bytes | -48 bytes |

The linked-runtime saving is small because parser, function metadata, and class
declarations remain for compatibility, but the archive still drops the
unsupported host-file I/O bodies from the default embedded profile.

## Test And Verification Plan

- Reconfigure and rebuild `build/mariadb-embedded` with the baseline profile.
- Reconfigure and rebuild `build/mariadb-mylite-storage-smoke` with
  `-DPLUGIN_MYLITE_SE=STATIC`.
- Confirm both build caches record `MYLITE_WITH_SQL_FILE_IO=OFF`.
- Rebuild and run embedded and storage-smoke tests covering SQL file-I/O
  policy.
- Build the normal MariaDB `sql` target with the same cache option to verify
  the non-embedded source list also compiles.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, and first-party tidy checks.

## Acceptance Criteria

- Default MyLite embedded profile builds with host-file SQL I/O bodies disabled.
- Direct and prepared file-I/O policy tests continue to pass with stable
  diagnostics.
- Size documentation records current branch evidence.
- Upstream-style MariaDB builds can retain host-file SQL I/O support by leaving
  `MYLITE_WITH_SQL_FILE_IO=ON`.

## Risks And Open Questions

- This slice keeps parser and builder metadata for link compatibility, so the
  trim is smaller than a full grammar/function-table removal.
- Internal bypasses should see unsupported stubs, not public MyLite errors.
  Public entry points must continue to use the policy gate.
