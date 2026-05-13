# System Variable Help Text Size Profile

## Problem

The aggressive MyLite minsize profile still retains MariaDB system-variable
help text. These strings describe command-line/server behavior for
`mariadbd --help` and `INFORMATION_SCHEMA.SYSTEM_VARIABLES`, including many
binlog, replication, network, log-file, and daemon-administration settings that
are disabled or low-value in the embedded runtime.

MyLite still needs system-variable names, values, defaults, validation, and
`SHOW VARIABLES`. It does not need long descriptive comments in the most
aggressive embedded profile.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/sys_vars.cc` declares the retained system
  variables and passes human-readable comments to the `Sys_var_*`
  constructors.
- `vendor/mariadb/server/sql/sys_vars.inl` defines the inline `Sys_var_*`
  constructors that forward those comments into the base `sys_var` object.
- `vendor/mariadb/server/sql/set_var.cc` stores the comment pointer in
  `sys_var::option.comment`.
- `fill_sysvars()` uses `option.comment` for the
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` column.
- Ordinary `SHOW VARIABLES` and `INFORMATION_SCHEMA.GLOBAL_VARIABLES` /
  `SESSION_VARIABLES` use names and value pointers, not the long comments.
- The earlier constructor-only prototype made
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` empty but left the
  original literals rooted at static constructor call sites, so it did not
  produce a useful linked-size win.

## Scope

This slice may:

- add `MYLITE_DISABLE_SYSVAR_HELP_TEXT`,
- enable it from `tools/build-mariadb-minsize.sh`,
- map system-variable comments to an empty string in the inline constructor
  path for the aggressive profile,
- wrap the `sys_vars.cc` static declaration comment arguments so the
  preprocessor can discard long literals before compilation,
- keep system-variable names, values, defaults, validation, command-line
  parsing, and `SHOW VARIABLES`,
- make `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` empty in the
  aggressive profile, and
- record smoke coverage and measured size deltas.

## Non-Goals

This slice does not:

- remove system variables,
- remove `SHOW VARIABLES`,
- remove `INFORMATION_SCHEMA.GLOBAL_VARIABLES`,
  `SESSION_VARIABLES`, or `SYSTEM_VARIABLES`,
- change variable defaults or update validation,
- remove `my_long_options[]` hardcoded option help text, or
- change non-minsize builds.

## Proposed Design

Add a minsize-only CMake option. When enabled, `sys_vars.cc` static
declarations should wrap each ordinary system-variable comment in
`MYLITE_SYSVAR_HELP_TEXT(...)`, which expands to `""` in the aggressive
profile and to the original text otherwise. The inline `Sys_var_*` constructors
also pass `""` to the base `sys_var` constructor in the aggressive profile so
engine or plugin system variables that still arrive through constructor
arguments expose empty comments too.

The declaration-site wrapper is the size-critical part: it lets the
preprocessor discard the long string tokens before compilation. Constructor
plumbing alone changes the stored pointer but can still leave the literal
addresses materialized by static initialization.

`fill_sysvars()` can remain unchanged because `option.comment` will be a valid
empty C string.

## Affected Subsystems

- Minsize CMake configuration.
- `Sys_var_*` constructor plumbing in `sys_vars.inl`.
- MyLite open/close smoke coverage.
- Production size analysis.

## Single-File and Embedded Lifecycle Impact

No file-format or storage lifecycle change. Startup option parsing and system
variable initialization remain intact.

## Public API and File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

SQL compatibility impact: in the aggressive minsize profile,
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty. Variable
names, values, scopes, defaults, and `SHOW VARIABLES` remain available.

## Binary-Size Impact

Measured on top of `status-metadata-size-profile`:

- `libmariadbd.a`: 27,417,704 -> 27,364,504 bytes (-53,200).
- stripped `mylite-open-close-smoke`: 5,227,600 -> 5,191,128 bytes
  (-36,472).
- stripped `mylite-compatibility-smoke`: 5,103,248 -> 5,065,160 bytes
  (-38,088).
- `.rodata` in `mylite-open-close-smoke`: 932,459 -> 895,979 bytes
  (-36,480).

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no new
dependency and changes no trademark-facing packaging.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sysvar-help-text MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sysvar-help-text tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sysvar-help-text tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sysvar-help-text tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-sysvar-help-text tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
- `bash -n tools/run-libmylite-open-close-smoke.sh`
- `git diff --check`

## Acceptance Criteria

- `SHOW VARIABLES LIKE 'version'` still returns the normal value row.
- `INFORMATION_SCHEMA.SYSTEM_VARIABLES` still exposes the `version` row.
- `VARIABLE_COMMENT` is empty for that row in the aggressive minsize profile.
- Current storage, bootstrap, and compatibility smokes still pass.
- Measured size changes are recorded in
  `docs/research/production-size-analysis.md`.

## Risks and Unresolved Questions

- Some clients may inspect `INFORMATION_SCHEMA.SYSTEM_VARIABLES` comments for
  UI/help rendering. This is acceptable only in the aggressive size profile.
- If a constructor path still forwards the original comment, some strings will
  remain rooted and the measured win may be smaller than expected.
- A small amount of hardcoded option help text in `mysqld.cc` remains. That is
  separate from system-variable comments and should be investigated as its own
  size slice.
