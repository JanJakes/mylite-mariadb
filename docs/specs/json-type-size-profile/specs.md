# JSON Type Size Profile

## Problem Statement

The aggressive MyLite minsize profile already omits `JSON_TABLE`,
`JSON_SCHEMA_VALID()`, and ordinary JSON SQL functions, but it still retains
MariaDB's JSON data-type alias layer. That keeps `sql_type_json.cc`,
`mylite_json_function_stub.cc`, JSON type-handler vtables, JSON-valid
constraint detection in string/blob fields, and parser-backed
`JSON_ARRAYAGG()` / `JSON_OBJECTAGG()` item stubs in the embedded archive.

This is useful compatibility for applications that declare `JSON` columns, but
it is a poor fit for the most aggressive size profile after JSON SQL execution
is already unavailable.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/sql_yacc.yy` maps `JSON` column syntax to
  `type_handler_long_blob_json` with `utf8mb4_bin`.
- `vendor/mariadb/server/sql/sql_type_json.cc` defines JSON string/blob type
  handlers, converts generic string handlers to JSON variants, injects
  `JSON_VALID()` check constraints for JSON columns, and provides the JSON
  type collection.
- `vendor/mariadb/server/sql/field.cc` detects `JSON_VALID()` constraints on
  string/blob fields and returns JSON-specific type handlers for metadata.
- `vendor/mariadb/server/sql/sql_select.cc` creates a `JSON_VALID()` virtual
  expression when temporary-table fields need to preserve JSON metadata.
- `vendor/mariadb/server/libmysqld/mylite_json_function_stub.cc` is retained
  in the JSON-functions-disabled profile for JSON type checks, JSON/string
  comparison helpers, and parser-backed JSON aggregate unsupported methods.
- The current linked smoke binary retains JSON type-handler vtables and
  `Item_func_json_valid` vtable/data even though scalar JSON functions are not
  registered.
- The current embedded archive contains `sql_type_json.cc.o` at 24,347 bytes
  and `mylite_json_function_stub.cc.o` at 20,842 bytes.

## Official Documentation References

- [MariaDB JSON Data Type](https://mariadb.com/kb/en/json-data-type/)
  documents `JSON` as a compatibility alias for `LONGTEXT COLLATE
  utf8mb4_bin`, with an automatic `JSON_VALID()` check constraint.
- [MariaDB JSON_VALID](https://mariadb.com/docs/server/reference/sql-functions/special-functions/json-functions/json_valid)
  documents the validation function and its relationship to the JSON data-type
  alias.
- [MariaDB JSON Functions](https://mariadb.com/kb/en/json-functions/) lists
  the broader JSON SQL surface that the aggressive minsize profile already
  treats as optional.

## Scope

This slice may:

- add `MYLITE_DISABLE_JSON_TYPE`,
- enable it only in the aggressive minsize build,
- reject `JSON` column declarations with an explicit unsupported diagnostic,
- reject parser-backed `JSON_ARRAYAGG()` and `JSON_OBJECTAGG()` before item
  construction in the same profile,
- make JSON type-handler detection return generic string/blob behavior,
- remove `sql_type_json.cc` and `mylite_json_function_stub.cc` from the
  embedded source list when the JSON type is disabled, and
- update smoke coverage and production-size analysis.

## Non-Goals

This slice does not:

- remove generic JSON parser/writer helpers still used by other retained
  subsystems,
- remove `JSON_TABLE` parser tokens or the existing `JSON_TABLE` unsupported
  stub,
- remove histogram JSON code or unrelated EXPLAIN JSON comments,
- change non-minsize builds,
- change the public `libmylite` API, or
- change `.mylite` file format.

## Proposed Design

Add a minsize-only CMake option, `MYLITE_DISABLE_JSON_TYPE`, that requires
`MYLITE_DISABLE_JSON_FUNCTIONS=ON`. The dependency avoids a partial state where
JSON functions execute while the `JSON` type alias cannot preserve MariaDB's
validation and metadata behavior.

When enabled:

- compile out `sql_type_json.cc` and `mylite_json_function_stub.cc`;
- provide tiny embedded stubs for the few declarations that remain reachable
  from headers or generic JSON comparison paths;
- make `Type_handler_json_common::is_json_type_handler()` always return false;
- guard `field.cc` JSON-check detection and `sql_select.cc` JSON temporary
  metadata preservation so generic string/blob handlers are used;
- reject `JSON` type syntax in `sql_yacc.yy` with
  `ER_NOT_SUPPORTED_YET`, mentioning the MyLite minsize profile; and
- reject `JSON_ARRAYAGG()` / `JSON_OBJECTAGG()` in the parser under the broad
  JSON-function-disabled profile, so the JSON aggregate item vtables and stub
  methods are no longer rooted.

## Affected Subsystems

- Embedded minsize CMake source selection.
- MariaDB parser-generated SQL grammar.
- SQL type handler metadata.
- Field metadata and temporary-table JSON preservation.
- Open/close smoke unsupported SQL coverage.
- Production size analysis.

## DDL Metadata Routing Impact

`CREATE TABLE ... (j JSON)` becomes explicitly unsupported in the aggressive
minsize profile. This prevents MyLite from storing a table definition whose
JSON validation expression cannot execute because JSON SQL functions are
absent. Existing string/blob columns and explicit non-JSON `CHECK` constraints
are unaffected by this slice.

## Single-File And Embedded-Lifecycle Impact

No storage file, catalog, recovery, locking, or sidecar-file behavior changes.
This removes SQL type metadata and expression code that does not own persistent
files.

## Public API Or File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

SQL compatibility impact is high for applications that declare MariaDB/MySQL
`JSON` columns. They can use `LONGTEXT`/`TEXT` columns in this profile if JSON
validation is handled outside SQL.

## Binary-Size Impact

Measured on top of the disabled server-option table row trim:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,991,050 | 25,577,556 | -413,494 |
| unstripped `mylite-open-close-smoke` | 6,693,368 | 6,639,904 | -53,464 |
| stripped `mylite-open-close-smoke` | 4,706,032 | 4,676,440 | -29,592 |
| `my_long_options` | 5,376 | 5,376 | 0 |

The embedded archive no longer contains `sql_type_json.cc.o` or
`mylite_json_function_stub.cc.o`; it contains a 460-byte
`mylite_json_type_stub.cc.o` for generic comparator fallbacks. The linked smoke
binary no longer contains representative JSON type-handler,
`Item_func_json_valid`, `Item_func_json_arrayagg`, or
`Item_func_json_objectagg` symbols.

## License, Trademark, And Dependency Impact

No new dependency or license impact. This is a GPL-2.0-only MariaDB-derived
build-profile change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

Measure:

- archive bytes,
- unstripped and stripped linked open-close smoke bytes,
- absence of `sql_type_json.cc.o` and `mylite_json_function_stub.cc.o` from
  `libmariadbd.a`, and
- absence of representative JSON type and JSON aggregate item symbols from the
  linked smoke binary.

## Acceptance Criteria

- The minsize build succeeds with `MYLITE_DISABLE_JSON_TYPE=ON`. Passed with
  `build/mariadb-minsize-no-json-type`.
- The open/close, storage-engine, and compatibility smokes pass. Passed.
- `CREATE TABLE ... JSON` fails with an explicit unsupported diagnostic.
  Passed.
- `JSON_ARRAYAGG()` and `JSON_OBJECTAGG()` still fail with explicit unsupported
  diagnostics. Passed.
- Existing JSON scalar functions remain rejected as unknown functions. Passed.
- `sql_type_json.cc.o` and `mylite_json_function_stub.cc.o` are absent from
  the embedded archive. Passed.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`. Passed.

## Verification

Validated on 2026-05-13 with:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-json-type \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The open/close report includes:

```text
exec_json_valid_message=FUNCTION JSON_VALID does not exist
exec_json_arrayagg_message=This version of MariaDB doesn't yet support 'JSON_ARRAYAGG in the MyLite minsize profile'
exec_json_objectagg_message=This version of MariaDB doesn't yet support 'JSON_OBJECTAGG in the MyLite minsize profile'
exec_json_type_message=This version of MariaDB doesn't yet support 'JSON data type in the MyLite minsize profile'
```

## Risks And Unresolved Questions

- This removes a common compatibility alias. It belongs only in the most
  aggressive size profile or as a committed experiment for later comparison.
- MariaDB can infer JSON metadata from `CHECK(JSON_VALID(...))` on string/blob
  columns; this profile deliberately stops publishing JSON metadata for such
  columns.
- Further JSON-related savings likely require pruning generic parser/writer
  helpers used by non-JSON features such as EXPLAIN or histograms.
