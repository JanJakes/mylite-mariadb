# Stored Function Lookup Size Profile

## Problem

The aggressive MyLite minsize profile rejects stored routine DDL and already
returns empty stored-routine metadata, but ordinary expression parsing still
treats unknown function calls as potential stored functions. That keeps
`Item_func_sp` and related stored-function lookup/runtime code live for queries
that MyLite cannot satisfy from a MyLite-owned routine catalog.

The current `build/mariadb-minsize-no-processlist` linked smoke still contains
stored-function call machinery such as `Item_func_sp`, `Item_sum_sp`, and
package-function call construction. Larger stored-program parser/runtime
objects also remain, but removing those requires a dedicated grammar slice.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/item_create.cc` falls back to
  `Create_sp_func` when a function name is not native, not a constructor, and
  not a UDF.
- `Create_sp_func::create_with_db()` creates `Item_func_sp` and registers used
  stored routines.
- `vendor/mariadb/server/sql/sql_lex.cc` creates `Item_func_sp` for
  package-qualified calls such as `db.pkg.func()`.
- `vendor/mariadb/server/sql/item_func.cc` and `sql_select.cc` implement
  `Item_func_sp` execution and temporary-field behavior.
- Stored routine DDL is already rejected for embedded builds in
  `mysql_execute_command()`, and the minsize profile already returns empty
  `INFORMATION_SCHEMA.ROUTINES` / `PARAMETERS`.

## Scope

This slice may:

- add `MYLITE_DISABLE_STORED_FUNCTION_LOOKUP`,
- enable it from `tools/build-mariadb-minsize.sh`,
- make unknown unqualified and qualified SQL functions report the existing
  MariaDB missing-function diagnostic instead of creating `Item_func_sp`,
- reject package-qualified function calls in the same way,
- compile out `Item_func_sp` method bodies that are only needed for stored
  function execution, and
- add smoke coverage for unqualified, schema-qualified, and package-qualified
  missing stored-function calls.

## Non-Goals

This slice does not:

- remove stored-program grammar,
- remove `sp_head`, `sp_instr`, `sp_pcontext`, or `sp_rcontext`,
- remove stored routine DDL rejection,
- remove routine Information Schema field descriptors,
- alter native SQL function lookup, or
- alter non-minsize builds.

## Proposed Design

Add a minsize-only CMake option. When enabled, replace the stored-function
fallback builder with a small missing-function builder that returns
`ER_SP_DOES_NOT_EXIST` without allocating `sp_name`, registering used routines,
or constructing `Item_func_sp`.

Guard the package-qualified function construction path in `sql_lex.cc` with
the same diagnostic. Guard `Item_func_sp` method definitions and its
`create_tmp_field_ex()` implementation so linked minsize artifacts can drop
the stored-function execution item when no remaining code constructs it.

## Affected Subsystems

- SQL expression function lookup in `item_create.cc`.
- Package-qualified function expression construction in `sql_lex.cc`.
- Stored-function item execution in `item_func.cc` and `sql_select.cc`.
- MyLite open/close unsupported-profile smoke coverage.
- Minsize CMake configuration.

## Single-File and Embedded Lifecycle Impact

No file-format or storage lifecycle change. The removed path would look up
stored functions in inherited MariaDB routine metadata, which MyLite does not
store in its current single-file catalog.

## Public API and File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

SQL compatibility impact: in the aggressive minsize profile, unknown function
calls no longer have a stored-function lookup path. They report the same
missing-function family of diagnostics without probing stored routine metadata.

## Binary-Size Impact

The implemented slice reduced `build/mariadb-minsize-no-stored-function-lookup`
against `build/mariadb-minsize-no-processlist` by:

- `libmariadbd.a`: 27,568,066 to 27,524,742 bytes, saving 43,324 bytes.
- unstripped `mylite-open-close-smoke`: 7,346,376 to 7,322,008 bytes, saving
  24,368 bytes.
- stripped `mylite-open-close-smoke`: 5,269,864 to 5,254,144 bytes, saving
  15,720 bytes.
- unstripped `mylite-compatibility-smoke`: 7,204,048 to 7,177,880 bytes,
  saving 26,168 bytes.
- stripped `mylite-compatibility-smoke`: 5,150,448 to 5,133,264 bytes, saving
  17,184 bytes.

`Item_func_sp`, `Item_sum_sp`, and `Create_sp_func` no longer appear in the
linked open-close smoke. Larger stored-program parser/runtime objects such as
`sp_head`, `sp_instr`, `sp_pcontext`, and `sp_rcontext` still remain and need a
separate grammar/runtime slice.

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no new
dependency and changes no trademark-facing packaging.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-stored-function-lookup MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-stored-function-lookup tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-stored-function-lookup tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-stored-function-lookup tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-stored-function-lookup tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
- `git diff --check`

All commands passed for the implemented slice. The first open-close smoke
attempt exposed unrelated missing vtables for `FOUND_ROWS()` and `SQLCODE`
because the initial method guard accidentally covered non-stored-function item
definitions; the final implementation narrows that guard and preserves those
methods.

## Acceptance Criteria

- Unknown unqualified SQL function calls still fail with a missing-function
  diagnostic.
- Unknown schema-qualified SQL function calls fail with a missing-function
  diagnostic without constructing `Item_func_sp`.
- Unknown package-qualified SQL function calls fail with a missing-function
  diagnostic without constructing `Item_func_sp`.
- Native SQL functions covered by the current smokes still work.
- Stored routine DDL rejection and empty routine Information Schema behavior
  still pass.
- Measured size changes are recorded in
  `docs/research/production-size-analysis.md`.

## Risks and Unresolved Questions

- This changes MariaDB's usual stored-function resolution path. It is coherent
  only while stored routines remain unsupported in the aggressive embedded
  profile.
- Generated parser actions still root broader stored-program parser/runtime
  code. The broader stored-program parser/runtime removal remains a separate
  high-risk slice.
