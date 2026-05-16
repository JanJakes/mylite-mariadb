# JSON Schema Validation Trim

## Problem

The default embedded profile still carries MariaDB's `JSON_SCHEMA_VALID()`
implementation and JSON-schema keyword registry. MyLite keeps ordinary JSON
helpers because real MySQL/MariaDB application schemas and upgrade SQL often
use functions such as `JSON_VALID()`, `JSON_EXTRACT()`, `JSON_SET()`, and
`JSON_VALUE()`. JSON schema validation is a narrower optional helper, and its
implementation pulls in a large schema-keyword class hierarchy plus startup
and shutdown hash initialization.

Keeping that path increases the embedded archive size without advancing the
single-file storage, embedded lifecycle, or application-schema roadmap.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt` and `mariadb/libmysqld/CMakeLists.txt` include
  `json_schema.cc` and `json_schema_helper.cc` in the SQL and embedded SQL
  source lists.
- `mariadb/sql/item_create.cc` declares `Create_func_json_schema_valid`,
  instantiates its singleton, creates `Item_func_json_schema_valid`, and
  registers `JSON_SCHEMA_VALID` in the native function registry.
- `mariadb/sql/item_jsonfunc.h` includes `json_schema.h` and declares
  `Item_func_json_schema_valid`, which owns `List<Json_schema_keyword>` state.
- `mariadb/sql/item_jsonfunc.cc` includes `json_schema_helper.h` and implements
  `Item_func_json_schema_valid::fix_length_and_dec()`, `val_bool()`, and
  `cleanup()`. Ordinary JSON functions in the same file also use
  `get_key_name()` from `json_schema_helper.cc` for JSON array/object hash
  operations.
- `mariadb/sql/mysqld.cc` calls `setup_json_schema_keyword_hash()` during
  library initialization and `cleanup_json_schema_keyword_hash()` during
  shutdown.
- Historical branch-level bundle-size research measured a comparable
  `JSON_SCHEMA_VALID()` trim as 44,912 linked bytes and 345,732 archive bytes
  saved while retaining ordinary JSON functions. The current profile must be
  remeasured because many server-surface trims have landed since that
  experiment.

## Design

- Add `MYLITE_WITH_JSON_SCHEMA_VALID`, defaulting to `ON` for normal MariaDB
  builds and forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, remove `json_schema.cc` from the SQL and embedded
  SQL source lists.
- Guard the `JSON_SCHEMA_VALID` builder, singleton, native function registry
  entry, item class declaration, item implementation, and schema keyword hash
  setup/cleanup calls behind `MYLITE_WITH_JSON_SCHEMA_VALID`.
- Keep `json_schema_helper.cc` because ordinary retained JSON functions use
  its hash-key helper.
- Reject direct and prepared MyLite SQL that calls `JSON_SCHEMA_VALID()` before
  MariaDB execution with a stable MyLite diagnostic.
- Keep ordinary JSON helpers, including `JSON_VALID()`, available.

## Affected Subsystems

- MariaDB SQL and embedded SQL build profile.
- Native function registration and JSON item implementation.
- Embedded runtime startup and shutdown.
- Public SQL policy and compatibility coverage.
- Size-profile documentation and measurement.

## MySQL/MariaDB Compatibility Impact

`JSON_SCHEMA_VALID()` is deliberately unsupported in the default MyLite
embedded profile. Ordinary JSON scalar/path functions remain in scope because
they are commonly used by application schemas, generated SQL, and MariaDB
system-table maintenance. Applications that depend on JSON schema validation
need a later compatibility decision before the function can be exposed.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change. Startup and shutdown must skip the JSON schema
keyword hash when the implementation is absent. Existing embedded lifecycle
tests must continue to prove repeated initialization and cleanup.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic that names `JSON_SCHEMA_VALID`.

## Storage-Engine Routing Impact

No direct storage-engine routing change. Routed table DDL/DML, generated
columns, CHECK constraints, and application-schema smoke coverage should
continue to use ordinary JSON functions where MariaDB needs them.

## Binary-Size Impact

Historical branch-level research suggests a meaningful archive reduction. This
slice records fresh default embedded and opt-in storage-smoke archive
measurements after implementation.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,414,744 bytes / 26.14 MiB | 675 | -104,760 bytes, -1 member |
| Storage-smoke | 27,595,328 bytes / 26.32 MiB | 678 | -104,760 bytes, -1 member |

The disabled default and storage-smoke embedded archives omit
`json_schema.cc.o` and retain `json_schema_helper.cc.o` for ordinary JSON hash
helpers used outside `JSON_SCHEMA_VALID()`.

## Implementation Notes

- `MYLITE_WITH_JSON_SCHEMA_VALID` defaults to `ON` for upstream-compatible
  builds and is forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- The disabled profile removes `json_schema.cc` from the SQL and embedded SQL
  source lists, while keeping `json_schema_helper.cc`.
- `item_create.cc` omits the `JSON_SCHEMA_VALID` builder, singleton, and
  registry entry in the disabled profile.
- `item_jsonfunc.h` and `item_jsonfunc.cc` omit the
  `Item_func_json_schema_valid` declaration and implementation in the disabled
  profile.
- `mysqld.cc` skips JSON schema keyword hash setup and cleanup in the disabled
  profile.
- Direct and prepared SQL policy rejects `JSON_SCHEMA_VALID()` before MariaDB
  execution with a stable `JSON_SCHEMA_VALID` diagnostic, while ordinary
  `JSON_VALID()` remains covered.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived schema-validation source
objects from the disabled embedded profile only and keeps normal MariaDB
defaults intact.

## Test And Verification Plan

- Add direct SQL policy coverage for `JSON_SCHEMA_VALID()` rejection.
- Add prepared statement coverage for `JSON_SCHEMA_VALID()` rejection.
- Add positive direct and prepared coverage proving ordinary `JSON_VALID()`
  remains available.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `json_schema.cc.o`.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Verification Results

Completed on 2026-05-16:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '^(json_schema|json_schema_helper)\.cc\.o$'`
- `ar -t build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a | rg '^(json_schema|json_schema_helper)\.cc\.o$'`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `cmake --preset embedded-dev`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `cmake --preset storage-smoke-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`

The archive scans printed only `json_schema_helper.cc.o` for both disabled
profiles, confirming that `json_schema.cc.o` is absent while the retained
ordinary JSON helper remains present.

## Acceptance Criteria

- Public direct and prepared SQL reject `JSON_SCHEMA_VALID()` before MariaDB
  execution with stable MyLite diagnostics.
- `JSON_VALID()` and ordinary retained JSON helpers continue to work in the
  default embedded profile.
- Default embedded and storage-smoke archives omit the disabled JSON schema
  validation object and record size reductions.
- Normal MariaDB builds keep the default `JSON_SCHEMA_VALID()` implementation.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Some applications may use `JSON_SCHEMA_VALID()` for application-level input
  validation. The current compatibility judgment is that this is narrower than
  ordinary JSON path/scalar helpers and can remain unsupported until a concrete
  application suite needs it.
- `json_schema.h` and `json_schema_helper.h` include each other in the upstream
  layout. This slice keeps the shared helper object rather than doing broad
  header surgery in the ordinary JSON implementation.
