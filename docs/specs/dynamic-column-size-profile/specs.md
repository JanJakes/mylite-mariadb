# Dynamic Column Size Profile

## Problem

The aggressive MyLite minsize profile still includes MariaDB dynamic-column SQL
functions and the `mysys` dynamic-column blob implementation. Dynamic columns
are a MariaDB-specific optional BLOB packing surface, not part of MyLite's
single-file storage engine, embedded lifecycle, or public `libmylite` API.

The current minsize build still links dynamic-column SQL item classes and
`mariadb_dyncol_*` helpers:

| Artifact | Bytes |
| --- | ---: |
| `build/mariadb-minsize-no-rpl-utility-server/libmysqld/libmariadbd.a` | 27,845,446 |
| `mysys/CMakeFiles/mysys.dir/ma_dyncol.c.o` | 45,000 |
| `sql/CMakeFiles/sql_embedded.dir/item_strfunc.cc.o` | 1,236,592 |
| `sql/CMakeFiles/sql_embedded.dir/item_cmpfunc.cc.o` | 1,097,432 |
| `sql/CMakeFiles/sql_embedded.dir/item_create.cc.o` | 838,504 |

The goal is to test whether MyLite can remove dynamic-column execution from
the minsize profile while keeping ordinary SQL, parser startup, storage, and
`libmylite` lifecycle behavior intact.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

MariaDB documents dynamic columns as a special-function family for storing
variable sets of columns in one BLOB, with functions such as `COLUMN_CREATE()`
and `COLUMN_GET()` and with client-side dynamic-column blob APIs in `libmysql`.
The current documentation pages are:

- <https://mariadb.com/docs/server/reference/sql-functions/special-functions/dynamic-columns-functions>
- <https://mariadb.com/docs/server/reference/sql-structure/nosql/dynamic-columns>

`vendor/mariadb/server/mysys/CMakeLists.txt` adds `ma_dyncol.c` to
`MYSYS_SOURCES`. That source defines the C helpers used by both SQL functions
and client compatibility exports, including:

- `mariadb_dyncol_create_many_num()`
- `mariadb_dyncol_create_many_named()`
- `mariadb_dyncol_update_many_num()`
- `mariadb_dyncol_update_many_named()`
- `mariadb_dyncol_get_num()`
- `mariadb_dyncol_get_named()`
- `mariadb_dyncol_list_num()`
- `mariadb_dyncol_list_named()`
- `mariadb_dyncol_check()`
- `mariadb_dyncol_json()`
- `mariadb_dyncol_unpack()`
- `mariadb_dyncol_column_count()`
- `mariadb_dyncol_free()`

`vendor/mariadb/server/sql/item_strfunc.h` and `.cc` define the string-returning
dynamic-column item classes:

- `Item_func_dyncol_create`
- `Item_func_dyncol_add`
- `Item_func_dyncol_json`
- `Item_dyncol_get`
- `Item_func_dyncol_list`

`vendor/mariadb/server/sql/item_cmpfunc.h` and `.cc` define boolean
dynamic-column item classes:

- `Item_func_dyncol_check`
- `Item_func_dyncol_exists`

`vendor/mariadb/server/sql/item_create.cc` registers native function builders
for `COLUMN_CHECK`, `COLUMN_EXISTS`, `COLUMN_LIST`, and `COLUMN_JSON`, and
provides parser helper factories for keyword-grammar functions:

- `create_func_dyncol_create()`
- `create_func_dyncol_add()`
- `create_func_dyncol_delete()`
- `create_func_dyncol_get()`

`vendor/mariadb/server/sql/sql_yacc.yy` keeps dedicated grammar for
`COLUMN_CREATE`, `COLUMN_ADD`, `COLUMN_DELETE`, and `COLUMN_GET`. This slice
does not remove those grammar productions because doing so requires a larger
parser regeneration and compatibility pass. Instead, the helpers should return
a stable unsupported diagnostic when dynamic columns are disabled.

`vendor/mariadb/server/sql/item.h` and `sql_type.h` include `ma_dyncol.h`
because dynamic-column type metadata is wired into the retained type handlers.
This slice therefore cannot remove the public type definitions without a much
larger type-system change.

## Scope

This slice may:

- add `MYLITE_DISABLE_DYNAMIC_COLUMNS`,
- enable it from `tools/build-mariadb-minsize.sh`,
- set Connector/C's `WITH_DYNCOL=OFF` in the aggressive minsize script,
- remove `ma_dyncol.c` from `mysys` in that profile,
- add a small `mysys` stub for retained dynamic-column C API symbols,
- omit dynamic-column SQL item classes and native function builders,
- make dedicated `COLUMN_CREATE` / `COLUMN_ADD` / `COLUMN_DELETE` /
  `COLUMN_GET` grammar helpers fail with `ER_NOT_SUPPORTED_YET`, and
- add smoke coverage proving dynamic columns fail predictably.

## Non-Goals

This slice does not:

- remove dynamic-column grammar tokens,
- remove `ma_dyncol.h` or dynamic-column enum/type metadata from SQL type
  handlers,
- preserve dynamic-column SQL or client C helper behavior in the aggressive
  minsize profile,
- change the public `libmylite` API,
- change the MyLite file format, or
- alter non-minsize builds.

## Proposed Design

Add `MYLITE_DISABLE_DYNAMIC_COLUMNS` as a global MyLite minsize CMake option so
both `mysys` and `libmysqld` can see it. When enabled:

- `mysys` removes `ma_dyncol.c` and compiles `mylite_dyncol_stub.c`;
- the stub defines the retained `mariadb_dyncol_*` and deprecated
  `dynamic_column_*` symbols with fail-closed behavior;
- `item_strfunc.h/.cc` and `item_cmpfunc.h/.cc` omit dynamic-column item
  classes;
- `item_create.cc` omits native dynamic-column builders and table entries;
- parser helper functions for `COLUMN_CREATE`, `COLUMN_ADD`, `COLUMN_DELETE`,
  and `COLUMN_GET` call `my_error(ER_NOT_SUPPORTED_YET, MYF(0),
  "dynamic columns in the MyLite minsize profile")` and return `NULL`.

Failing closed is intentional. Dynamic columns are a data-encoding feature. If
a hidden retained path accidentally calls the C API, returning an error is safer
than manufacturing a partial blob format.

## Affected Subsystems

- `mysys` dynamic-column C helpers.
- SQL native-function registry.
- SQL parser helper factories for dynamic-column keyword functions.
- SQL item class declarations and definitions.
- `libmylite` open/close smoke unsupported-profile checks.

## Single-File and Embedded Lifecycle Impact

Dynamic columns do not own MariaDB sidecar files directly, so this change has no
file-format or storage lifecycle impact. It aligns with the embedded minsize
profile by removing a rare SQL/blob helper surface unrelated to opening and
owning one `.mylite` file.

## Public API and File-Format Impact

No MyLite public API change and no file-format change.

The inherited embedded MariaDB client API still exports the dynamic-column
symbol names in static-link contexts, but the aggressive minsize profile changes
their behavior to return dynamic-column error codes instead of packing or
unpacking dynamic-column blobs. That is a compatibility tradeoff specific to
this profile.

## Binary-Size Impact

The direct object upper bound is the removable `ma_dyncol.c.o` plus portions of
`item_strfunc.cc.o`, `item_cmpfunc.cc.o`, and `item_create.cc.o`. Linked-runtime
savings may be smaller because section garbage collection already drops some
unreachable methods and because the parser/type system still keeps dynamic
column metadata definitions.

Measured result on top of `rpl-utility-server-size-profile`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 27,845,446 | 27,669,358 | -176,088 |
| unstripped `mylite-open-close-smoke` | 7,430,128 | 7,373,312 | -56,816 |
| stripped `mylite-open-close-smoke` | 5,337,144 | 5,294,680 | -42,464 |
| unstripped `mylite-compatibility-smoke` | 7,295,152 | 7,236,280 | -58,872 |
| stripped `mylite-compatibility-smoke` | 5,224,096 | 5,179,824 | -44,272 |

The object-level changes explain the archive and linked savings:

| Object | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `item_strfunc.cc.o` | 1,236,592 | 1,146,304 | -90,288 |
| `item_cmpfunc.cc.o` | 1,097,432 | 1,070,376 | -27,056 |
| `item_create.cc.o` | 838,504 | 818,328 | -20,176 |
| `ma_dyncol.c.o` / `mylite_dyncol_stub.c.o` | 45,000 | 7,752 | -37,248 |

`size` reports the removed `ma_dyncol.c.o` at 16,525 bytes of text/data/bss and
the replacement stub at 500 bytes. The linked smoke no longer contains
`Item_func_dyncol_*` or `Item_dyncol_get` symbols; the previous build had 97
dynamic-column item symbols.

The minsize script also sets Connector/C's `WITH_DYNCOL=OFF`. That setting does
not change the measured `mysqlserver`/`libmariadbd.a` target above, but keeps
accidentally built client-library targets aligned with the same profile.

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no new
third-party dependency and changes no public trademark-facing packaging.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
- `git diff --check`

Also verify that `libmysys.a` no longer contains `ma_dyncol.c.o` and instead
contains the MyLite dynamic-column stub object.

Executed verification:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-dynamic-columns tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
- `git diff --check`

GNU `ar -t` verifies `libmysys.a` contains `mylite_dyncol_stub.c.o` with no
`ma_dyncol.c.o`.

## Acceptance Criteria

- The aggressive minsize build passes.
- The relevant smoke tests and compatibility harness pass.
- Dynamic-column SQL functions fail predictably in the open/close smoke.
- `ma_dyncol.c.o` is absent from the `mysys` archive when the option is
  enabled.
- Size deltas are recorded in this spec and in production size analysis.
- Non-minsize behavior remains unchanged because the option defaults to `OFF`.

## Risks and Unresolved Questions

The main risk is that retained SQL type metadata or a compatibility C API path
still expects fully working dynamic-column helpers. The smoke and compatibility
harness should catch ordinary SQL regressions, but direct third-party use of
the embedded MariaDB dynamic-column C helpers is intentionally not preserved in
the aggressive MyLite minsize profile.

Removing the dynamic-column grammar tokens themselves is left for a later parser
slice because it is parser-table work with a different review surface and likely
smaller marginal savings.
