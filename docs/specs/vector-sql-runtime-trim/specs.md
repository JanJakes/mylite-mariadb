# Vector SQL Runtime Trim

## Goal

Remove MariaDB's vector SQL function and MHNSW vector-index runtime from the
default embedded profile while keeping the rest of the application SQL surface
unchanged.

## Non-Goals

- Do not remove JSON, GEOMETRY/GIS, ordinary scalar functions, or native
  storage engines.
- Do not remove `VECTOR(N)` type parsing in this slice.
- Do not design a MyLite vector-search storage path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/item_vectorfunc.cc` implements `VEC_FROMTEXT()`,
  `VEC_TOTEXT()`, `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and
  `VEC_DISTANCE_COSINE()`.
- `mariadb/sql/item_create.cc` registers those functions in the native
  function registry.
- `mariadb/sql/vector_mhnsw.cc` implements the mandatory `mhnsw` plugin,
  vector-index transaction participant, cache, index maintenance, and
  `mhnsw_*` entry points used by retained SQL/table/handler code.
- `mariadb/sql/sql_builtin.cc.in` hardcodes the `mhnsw` plugin in the
  mandatory built-in plugin list.
- Retained MariaDB code in `handler.cc`, `sql_base.cc`, `sql_table.cc`,
  `sql_show.cc`, `table.cc`, and `sys_vars.cc` still references `mhnsw_*`
  declarations, so the disabled profile needs fail-closed link stubs instead
  of deleting the API surface wholesale.

## Compatibility Impact

MariaDB vector search is a newer optional search surface, not part of the
current MyLite compatibility target. The default profile rejects public direct
and prepared `VEC_*` calls before MariaDB dispatch. Vector-index DDL remains
unsupported and fails without publishing an application table. `VECTOR(N)` type
parsing stays linked as a separate compatibility decision.

## Design

Add `MYLITE_WITH_VECTOR_SQL_RUNTIME`, defaulting to `ON` for inherited MariaDB
builds and forced `OFF` by the MyLite embedded baseline.

When disabled:

- omit `item_vectorfunc.cc` from embedded and server SQL source lists;
- replace `vector_mhnsw.cc` with `mylite_vector_sql_runtime_disabled.cc`;
- avoid registering `VEC_*` native function builders;
- avoid registering the mandatory `mhnsw` built-in plugin;
- reject vector-index creation before assigning the MHNSW index plugin; and
- reject `VEC_*` calls in the `libmylite` SQL policy with a MyLite-owned
  diagnostic.

The disabled source preserves retained `mhnsw_*` symbols and returns
`ER_NOT_SUPPORTED_YET` if a retained internal path reaches vector-index code.

## File Lifecycle

The disabled profile does not add durable, temporary, lock, metadata, cache, or
runtime files. It removes the only default-profile path that could create
MHNSW vector-index companion state.

## Embedded Lifecycle And API

No C API shape changes. Direct execution and prepared statement preparation
return `MYLITE_ERROR` with a diagnostic that names the unsupported vector SQL
runtime. Opening, closing, native storage setup, and ordinary statement
lifecycle behavior are unchanged.

## Build, Size, And Dependencies

No new dependencies or license changes. The slice removes MariaDB-derived
runtime objects from the default embedded archive only.

| Profile | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| Previous archive | 26,020,528 | 24.82 | 693 |
| After vector trim and corrected Darwin strip | 25,937,816 | 24.74 | 692 |

The pre-strip vector runtime omission saves 91,752 bytes. The final stripped
archive is 82,712 bytes smaller and removes one archive member after the
clean-build Darwin strip policy correction.

## Test Plan

Run:

```sh
tools/mariadb-embedded-build all
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

Additional checks:

- the embedded archive reports `MYLITE_WITH_VECTOR_SQL_RUNTIME:BOOL=OFF`;
- `item_vectorfunc.cc.o` and `vector_mhnsw.cc.o` are absent from the embedded
  archive;
- `mylite_vector_sql_runtime_disabled.cc.o` is present;
- linked symbols no longer expose `Create_func_vec_*`, `Item_func_vec_*`,
  `MHNSW_Share`, or `builtin_maria_mhnsw_plugin`; and
- direct, prepared, and DDL coverage prove the public unsupported boundary.

## Acceptance Criteria

- Public direct and prepared vector SQL calls fail through MyLite policy.
- Quoted mentions of vector function names remain ordinary string literals.
- Vector-index DDL fails and does not create an application table.
- JSON, GEOMETRY/GIS, ordinary scalar functions, DDL/DML, transactions,
  prepared statements, and native storage coverage still pass.
- Docs record the unsupported boundary and measured archive impact.

## Risks And Open Questions

- Applications that require MariaDB vector search need a separate compatibility
  profile or a future MyLite vector storage design.
- A later `VECTOR(N)` type-handler cut may save additional bytes, but it is a
  stronger compatibility decision and is deliberately not included here.
