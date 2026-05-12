# Geometry Type Size Profile

## Problem

The aggressive MyLite minsize profile now omits GIS SQL functions, MyISAM RTREE
support, and MariaDB's spatial WKB/WKT core. The embedded runtime still links
`sql_type_geom.cc`, which defines GEOMETRY type handlers, geometry field
materialization, GEOMETRY metadata packing/unpacking, and spatial key helpers.
MyLite storage rejects GEOMETRY columns and SPATIAL keys, so the remaining type
implementation is mostly compatibility scaffolding.

Current measured geometry type object size from
`build/mariadb-minsize-sql-sequence`:

| Object | File bytes | `size` total |
| --- | ---: | ---: |
| `libmysqld/.../sql_type_geom.cc.o` | 431,736 | 66,876 |

## Source Findings

MariaDB source references are from imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `libmysqld/CMakeLists.txt` includes `../sql/sql_type_geom.cc` and
  `../sql/sql_type_geom.h` in `SQL_EMBEDDED_SOURCES`.
- `sql/sql_type_geom.cc` defines the global `type_handler_geometry`,
  geometry subtype handlers, `type_collection_geometry`,
  `Type_collection_geometry_handler_by_name()`, `Type_handler_geometry`
  methods, and `Field_geom` methods.
- After the GIS-function and spatial-core profiles, remaining non-geometry
  embedded objects reference only `type_handler_geometry`,
  `type_collection_geometry`, `Type_collection_geometry::init()`, and
  `Type_collection_geometry_handler_by_name()`.
- `sql/sql_type.cc` needs the geometry type collection during type-aggregator
  initialization and still maps `MYSQL_TYPE_GEOMETRY` to
  `type_handler_geometry` for retained generic metadata paths.
- `mylite_gis_function_stub.cc` returns `type_handler_geometry` for the
  retained empty GIS constructor/type registry shims.
- `storage/mylite/ha_mylite.cc` rejects `MYSQL_TYPE_GEOMETRY` fields and
  `HA_SPATIAL_legacy` keys before MyLite accepts table definitions.

## Scope

Add a `MYLITE_DISABLE_GEOMETRY_TYPE` aggressive minsize option that:

- requires `MYLITE_DISABLE_GIS_FUNCTIONS=ON` and
  `MYLITE_DISABLE_SPATIAL_CORE=ON`,
- removes `../sql/sql_type_geom.cc` from the embedded SQL source list,
- links a small embedded-only stub for the retained global geometry type
  symbols expected by `sql_type.cc` and GIS stubs,
- stops mapping GEOMETRY type names through
  `Type_collection_geometry_handler_by_name()`, and
- makes any retained geometry field/materialization path fail explicitly.

## Non-Goals

This slice does not remove `MYSQL_TYPE_GEOMETRY` enum handling from generic
protocol, replication, parser, or metadata code.

This slice does not remove GEOMETRY grammar tokens or all GEOMETRY mentions in
diagnostics.

This slice does not implement GEOMETRY storage, SPATIAL indexes, or GIS
functions.

This slice does not change the full MariaDB server target.

## Binary-Size Impact

Expected static archive savings are bounded by the 431,736-byte
`sql_type_geom.cc.o` object, minus the replacement stub. Expected linked
runtime savings are bounded by the object's 66,876 bytes of allocated sections
before link-layout effects.

## DDL Metadata Routing Impact

No MyLite catalog-format change. MyLite must continue to avoid persisting
GEOMETRY columns and SPATIAL keys. Depending on where the retained parser routes
GEOMETRY names after the stub, GEOMETRY DDL may fail as an unknown type or as a
MyLite unsupported storage feature; either result is acceptable if no table is
created.

## Single-File And Embedded-Lifecycle Implications

No file-format, catalog, lock, recovery, or lifecycle changes are expected. The
compatibility harness must continue to verify that unsupported GEOMETRY/SPATIAL
DDL leaves no MyLite table behind and that sidecar scans remain clean.

## Public API Or File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## License, Trademark, And Dependency Impact

No new dependency or licensing change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-geometry-type MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-geometry-type MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-geometry-type MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh tools/run-compatibility-test-harness.sh
```

Measure:

- `libmysqld/libmariadbd.a` bytes and object count,
- stripped `mylite-open-close-smoke` bytes,
- absence of `sql_type_geom.cc.o` from the minsize archive,
- absence of `Field_geom` and concrete geometry subtype handler symbols from
  the linked smoke, and
- retained GEOMETRY/SPATIAL rejection behavior in the compatibility harness.

## Acceptance Criteria

- Default minsize build links without `sql_type_geom.cc.o`.
- Open/close smoke and full compatibility harness pass.
- Unsupported GEOMETRY and SPATIAL DDL leave no MyLite table behind.
- No GIS function or spatial-core implementation symbols are reintroduced.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.

## Risks

- This is a high-compatibility cut. GEOMETRY type names may become unknown
  rather than recognized-then-rejected by MyLite.
- Generic metadata code still keeps `MYSQL_TYPE_GEOMETRY` enum cases, so this
  is not a full type-system removal.
- The replacement stub must still satisfy `type_handler_geometry` vtable
  expectations without constructing `Field_geom`.
