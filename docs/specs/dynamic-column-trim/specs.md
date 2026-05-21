# Dynamic Column Trim

## Goal

Remove MariaDB dynamic-column SQL helpers and standalone dynamic-column helper
runtime from the default embedded profile without changing ordinary SQL, JSON,
GEOMETRY/GIS, native storage, or database-directory behavior.

## Non-Goals

- Do not remove JSON functions, JSON column aliases, or JSON histogram internals.
- Do not remove GEOMETRY/GIS types, functions, or spatial metadata.
- Do not remove ordinary scalar functions, DDL/DML, transactions, prepared
  statements, or native storage engines.
- Do not remove the internal type-handler `dyncol_type()` methods unless a later
  source review proves they are unnecessary outside dynamic-column execution.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `COLUMN_ADD()`, `COLUMN_DELETE()`,
  `COLUMN_CREATE()`, and `COLUMN_GET()` through dedicated grammar rules that
  call `create_func_dyncol_add()`, `create_func_dyncol_delete()`,
  `create_func_dyncol_create()`, and `create_func_dyncol_get()`.
- `mariadb/sql/item_create.cc` registers `COLUMN_CHECK()`,
  `COLUMN_EXISTS()`, `COLUMN_LIST()`, and `COLUMN_JSON()` in the native
  function registry, and also implements the parser-called `create_func_dyncol_*`
  helpers.
- `mariadb/sql/item_strfunc.cc` implements `Item_func_dyncol_create`,
  `Item_func_dyncol_add`, `Item_dyncol_get`, `Item_func_dyncol_json`, and
  `Item_func_dyncol_list`.
- `mariadb/sql/item_cmpfunc.cc` implements `Item_func_dyncol_check` and
  `Item_func_dyncol_exists`.
- `mariadb/mysys/ma_dyncol.c` implements the standalone dynamic-column C helper
  API declared by `mariadb/include/ma_dyncol.h`. The embedded archive currently
  contains `ma_dyncol.c.o`.
- Dynamic columns are MariaDB-specific helper functions rather than MySQL
  compatibility behavior. They are not part of the current MyLite compatibility
  matrix or the representative application-schema coverage.

## Compatibility Impact

Dynamic-column SQL helpers become explicitly unsupported in the default
embedded profile. This affects `COLUMN_CREATE()`, `COLUMN_ADD()`,
`COLUMN_DELETE()`, `COLUMN_GET()`, `COLUMN_CHECK()`, `COLUMN_EXISTS()`,
`COLUMN_LIST()`, and `COLUMN_JSON()`.

The change does not affect ordinary SQL expressions, JSON functions,
GEOMETRY/GIS, DDL/DML, transactions, prepared statements, result metadata,
diagnostics, or native storage engines.

Applications that require MariaDB dynamic columns need a separate compatibility
profile or a future MyLite decision to support them.

## Design

Add `MYLITE_WITH_DYNAMIC_COLUMNS`, defaulting to `ON` for inherited MariaDB
builds and forced `OFF` by the MyLite embedded baseline.

When disabled:

- compile the dynamic-column SQL item bodies out of `item_strfunc.cc` and
  `item_cmpfunc.cc`;
- remove `COLUMN_CHECK()`, `COLUMN_EXISTS()`, `COLUMN_LIST()`, and
  `COLUMN_JSON()` native function builders from the registry;
- keep the parser-called `create_func_dyncol_*` symbols as fail-closed stubs;
- replace `ma_dyncol.c` with a small fail-closed C helper implementation so
  inherited dynamic-column C API symbols remain linkable but unsupported; and
- reject direct and prepared `COLUMN_*` dynamic-column calls in `libmylite`
  policy with a MyLite-owned diagnostic.

## File Lifecycle

No durable, temporary, lock, metadata, cache, or runtime files are added,
removed, or relocated. Dynamic columns are in-memory expression/helper runtime,
not database-directory state.

## Embedded Lifecycle And API

No `libmylite` public API shape changes. Direct execution and prepared
statement preparation return `MYLITE_ERROR` with a diagnostic naming unsupported
dynamic columns. Opening, closing, native storage setup, and ordinary statement
lifecycle behavior are unchanged.

The inherited MariaDB dynamic-column C helper API remains outside the primary
`libmylite` API. In the disabled embedded profile, those helper entry points
fail closed instead of encoding or decoding dynamic-column values.

## Build, Size, And Dependencies

No new dependencies or license changes. The slice removes MariaDB-derived
runtime code from the default embedded archive only.

| Profile | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| Previous archive | 25,723,176 | 24.53 | 691 |
| After dynamic-column trim | 25,635,600 | 24.45 | 691 |

The pre-strip dynamic-column omission saves 88,808 bytes. The final stripped
archive is 87,576 bytes smaller with no archive-member count change because the
full helper implementation is replaced by a small fail-closed object.

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

- the embedded archive reports `MYLITE_WITH_DYNAMIC_COLUMNS:BOOL=OFF`;
- `ma_dyncol.c.o` is absent from the embedded archive and the disabled helper
  object is present;
- dynamic-column item and builder symbols are absent or fail closed in the
  embedded archive;
- direct and prepared dynamic-column SQL helper calls fail through MyLite
  policy; and
- quoted mentions of dynamic-column helper names remain ordinary string
  literals.

## Acceptance Criteria

- Public direct and prepared dynamic-column SQL helper calls fail through
  MyLite policy.
- Quoted mentions of dynamic-column helper names remain ordinary string
  literals.
- JSON, GEOMETRY/GIS, ordinary scalar functions, DDL/DML, transactions,
  prepared statements, and native storage coverage still pass.
- Docs record the unsupported boundary and measured archive impact.

## Risks And Open Questions

- Dynamic columns are MariaDB-specific, but some MariaDB applications may use
  them. The default embedded profile should keep the unsupported boundary
  explicit rather than letting calls fail as unknown functions.
