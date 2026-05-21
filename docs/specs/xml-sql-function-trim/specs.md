# XML SQL Function Trim

## Goal

Remove MariaDB's legacy XML XPath SQL helper runtime from the default embedded
profile without changing ordinary SQL, JSON, GEOMETRY/GIS, native storage, or
the already documented `LOAD XML` host-file import boundary.

## Non-Goals

- Do not remove JSON, GEOMETRY/GIS, ordinary scalar functions, or native
  storage engines.
- Do not remove charset XML metadata files or charset loading behavior.
- Do not broaden the already unsupported `LOAD XML` host-file import surface.
- Do not remove internal XML helper code that retained MariaDB startup or
  charset paths still need.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/item_xmlfunc.cc` implements MariaDB's XPath evaluator and the
  `Item_func_xml_extractvalue` / `Item_func_xml_update` runtime used by
  `EXTRACTVALUE()` and `UPDATEXML()`.
- `mariadb/sql/item_create.cc` defines `Create_func_xml_extractvalue` and
  `Create_func_xml_update`, then registers `EXTRACTVALUE` and `UPDATEXML` in
  the native function registry.
- `mariadb/sql/item.h` includes `item_xmlfunc.h`, so the disabled profile only
  needs to omit builders and the implementation object; no retained link path
  needs fail-closed item symbols.
- `mariadb/sql/sql_load.cc` has separate `LOAD XML` parsing and row-reading
  code. MyLite already rejects `LOAD XML` before dispatch and omits host-file
  import execution; this slice does not rely on or alter that boundary.
- Current first-party embedded test binaries do not link LibXml2; the XML SQL
  helper source uses MariaDB's internal `my_xml` parser instead.

## Compatibility Impact

`EXTRACTVALUE()` and `UPDATEXML()` are legacy MariaDB/MySQL XML XPath helper
functions. They are outside the current embedded compatibility target because
they are not core DDL/DML, transactions, native storage, JSON, GEOMETRY/GIS, or
ordinary scalar SQL. The default profile rejects direct and prepared calls
through MyLite policy with a stable diagnostic.

Applications that depend on these XML helpers need a separate compatibility
profile or a future MyLite decision to support them.

## Design

Add `MYLITE_WITH_XML_SQL_FUNCTIONS`, defaulting to `ON` for inherited MariaDB
builds and forced `OFF` by the MyLite embedded baseline.

When disabled:

- omit `item_xmlfunc.cc` from embedded and server SQL source lists;
- avoid defining and registering the `EXTRACTVALUE()` and `UPDATEXML()` native
  function builders; and
- reject direct and prepared `EXTRACTVALUE()` / `UPDATEXML()` calls in the
  `libmylite` SQL policy with a MyLite-owned diagnostic.

## File Lifecycle

No new durable, temporary, lock, metadata, cache, or runtime files are added or
removed. The disabled SQL functions do not own MyLite database-directory state.

## Embedded Lifecycle And API

No C API shape changes. Direct execution and prepared statement preparation
return `MYLITE_ERROR` with a diagnostic naming unsupported XML SQL functions.
Opening, closing, native storage setup, and ordinary statement lifecycle
behavior are unchanged.

## Build, Size, And Dependencies

No new dependencies or license changes. The slice removes MariaDB-derived
runtime objects from the default embedded archive only.

| Profile | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| Previous archive | 25,937,816 | 24.74 | 692 |
| After XML helper trim | 25,723,176 | 24.53 | 691 |

The pre-strip XML helper omission saves 217,592 bytes. The final stripped
archive is 214,640 bytes smaller and removes one archive member.

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

- the embedded archive reports `MYLITE_WITH_XML_SQL_FUNCTIONS:BOOL=OFF`;
- `item_xmlfunc.cc.o` is absent from the embedded archive;
- direct and prepared XML SQL helper calls fail through MyLite policy; and
- quoted mentions of XML helper names remain ordinary string literals.

## Acceptance Criteria

- Public direct and prepared XML SQL helper calls fail through MyLite policy.
- Quoted mentions of `EXTRACTVALUE()` and `UPDATEXML()` remain ordinary string
  literals.
- JSON, GEOMETRY/GIS, ordinary scalar functions, DDL/DML, transactions,
  prepared statements, and native storage coverage still pass.
- Docs record the unsupported boundary and measured archive impact.

## Risks And Open Questions

- Some legacy applications may use these XML helpers. The default embedded
  profile should keep the unsupported boundary explicit rather than letting
  calls fail as unknown functions.
