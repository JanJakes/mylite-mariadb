# GIS SQL Function Trim

## Problem

MyLite currently rejects unsupported `GEOMETRY` table shapes and `SPATIAL`
indexes, but the default embedded profile still links MariaDB's GIS SQL
function registry and geometry calculation helpers. That keeps constructor,
conversion, predicate, measurement, and set-operation functions in the embedded
archive even though MyLite cannot yet persist geometry values or implement
spatial access paths.

The bundle-size research records a successful branch experiment for omitting
GIS SQL function sources while keeping the geometry type code needed by current
parser and DDL rejection paths. This slice turns that evidence into an explicit
default embedded profile option and stable public SQL policy.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt:144` includes `item_geofunc.cc` in the normal
  SQL library.
- `mariadb/sql/CMakeLists.txt:205` includes `gcalc_slicescan.cc` and
  `gcalc_tools.cc` in the normal SQL library.
- `mariadb/libmysqld/CMakeLists.txt:104` includes `../sql/item_geofunc.cc` in
  the embedded `sql_embedded` archive.
- `mariadb/libmysqld/CMakeLists.txt:149` includes `../sql/gcalc_tools.cc` and
  `../sql/gcalc_slicescan.cc` in the embedded `sql_embedded` archive.
- `mariadb/sql/item_create.cc:64` declares
  `native_func_registry_array_geom`, and `item_create_init()` appends it to the
  native-function hashes at `mariadb/sql/item_create.cc:6785-6808`.
- `mariadb/sql/item_geofunc.cc:3948-4108` defines the GIS native function
  registry, including aliases such as `ST_ASTEXT`, `ST_GEOMFROMTEXT`,
  `ST_CONTAINS`, `AREA`, `POINTFROMTEXT`, `X`, and `Y`.
- `mariadb/sql/item_geofunc.cc:40-47`,
  `mariadb/sql/item_geofunc.cc:897-917`, and
  `mariadb/sql/item_geofunc.cc:930-938` define small geometry item methods
  still referenced by retained geometry type constructor paths.
- `mariadb/sql/gcalc_tools.cc:608-616` defines
  `Gcalc_result_receiver::get_result_typeid()`, which is referenced by retained
  headers and needs a small link shim when GIS operations are not built.
- `mariadb/sql/sql_type_geom.cc:377-489` retains geometry type-cast and type
  constructor wiring through `Item_func_geometry_from_text`,
  `Item_func_point`, and spatial collection item classes.
- `mariadb/sql/sql_type_geom.cc` and `mariadb/sql/spatial.cc` implement
  geometry type metadata plus WKB/WKT parsing helpers. They are retained in this
  slice so current type recognition and DDL rejection behavior remain intact.
- `mariadb/storage/mylite/ha_mylite.cc:2144-2152` rejects unsupported key
  classes including `HA_SPATIAL_legacy`.
- `packages/libmylite/tests/embedded_storage_engine_test.c:1435-1447` covers
  SPATIAL-index DDL rejection before MyLite catalog publication.
- The current default embedded archive contains `item_geofunc.cc.o`,
  `gcalc_tools.cc.o`, `gcalc_slicescan.cc.o`, `sql_type_geom.cc.o`, and
  `spatial.cc.o`.

Official MariaDB references:

- <https://mariadb.com/docs/server/reference/sql-structure/geometry>
- <https://mariadb.com/kb/en/st_astext/>

## Scope

- Add `MYLITE_WITH_GIS_SQL_FUNCTIONS`, defaulting to `ON`.
- Set `MYLITE_WITH_GIS_SQL_FUNCTIONS=OFF` in the MyLite embedded profile.
- When the option is off, omit `item_geofunc.cc`, `gcalc_tools.cc`, and
  `gcalc_slicescan.cc` from both the normal `sql` target and embedded
  `sql_embedded` archive.
- Add a MyLite-owned MariaDB source shim that defines an empty
  `native_func_registry_array_geom` plus the small geometry item/result shims
  needed by retained geometry type code.
- Reject public direct and prepared SQL calls to MariaDB GIS SQL functions
  before MariaDB execution with a stable MyLite diagnostic.

## Non-Goals

- Remove `GEOMETRY`, `POINT`, `LINESTRING`, `POLYGON`, or other geometry type
  parsing.
- Remove `sql_type_geom.cc`, `spatial.cc`, or geometry metadata needed by
  current DDL rejection paths.
- Remove `SPATIAL` index syntax.
- Implement geometry storage, geometry scalar evaluation, or SPATIAL access
  paths in MyLite.
- Remove the `type_geom` plugin or geometry SQL type handlers.

## Design

Define `MYLITE_WITH_GIS_SQL_FUNCTIONS` in `mariadb/sql/CMakeLists.txt` and
`mariadb/libmysqld/CMakeLists.txt`. Use a `MYLITE_GIS_SQL_FUNCTION_SOURCES`
source variable so the default upstream-like configuration still builds:

- `item_geofunc.cc`;
- `gcalc_tools.cc`;
- `gcalc_slicescan.cc`.

When the option is off, replace those sources with a MyLite-owned
`mylite_gis_sql_functions_disabled.cc` source. The disabled source should:

- define an empty `Native_func_registry_array native_func_registry_array_geom`;
- keep `Item_geometry_func::fix_length_and_dec()` link-compatible;
- make `Item_func_point::val_str()` and
  `Item_func_spatial_collection::val_str()` fail closed if reached;
- provide a minimal `Gcalc_result_receiver::get_result_typeid()` shim for link
  completeness.

In `libmylite`, add a policy scanner for GIS SQL function calls. The scanner
should use the existing token scanner pattern, require an opening parenthesis
after the token, and skip quoted strings/comments. The match list should cover
the MariaDB GIS native registry names that remain unavailable when the registry
is empty, including representative `ST_*` functions and legacy aliases such as
`ASTEXT`, `GEOMFROMTEXT`, `POINTFROMWKB`, `X`, and `Y`. It should also cover
retained geometry type-constructor calls such as `Point()` that would otherwise
reach the fail-closed shim.

## Compatibility Impact

GIS SQL functions become an explicit unsupported SQL-function family in the
core embedded profile. This includes representative constructor, conversion,
predicate, measurement, and set-operation functions such as `ST_AsText()`,
`ST_GeomFromText()`, `ST_Contains()`, `ST_Buffer()`, `PointFromText()`,
`Point()`, and `X()`.

This is a significant compatibility tradeoff. It is acceptable for the current
default profile because MyLite cannot yet store geometry values or support
SPATIAL access paths, and current application-schema coverage does not depend
on GIS. When MyLite implements geometry storage, this profile choice must be
revisited.

## DDL Metadata Routing Impact

Geometry column and SPATIAL-index DDL behavior must not regress. Unsupported
SPATIAL indexes must still reject before MyLite catalog publication, and the
slice must not allow unsupported geometry table definitions to persist or create
durable MariaDB sidecars.

## Single-File And Embedded-Lifecycle Impact

No durable file, sidecar, startup, shutdown, lock, recovery, or cleanup behavior
changes. The trim removes unreachable GIS function execution code from the
default embedded profile.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes. Public SQL execution and
prepare APIs return stable MyLite diagnostics for unsupported GIS SQL function
calls.

## Storage-Engine Routing Impact

No routed engine behavior changes. Existing SPATIAL-index rejection and current
geometry table-shape rejection coverage remain the storage-facing guardrails.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. Future compatibility profiles or adapters can enable
the option if an integration needs MariaDB GIS SQL functions.

## Binary-Size Impact

`docs/architecture/bundle-size-research.md` records prior evidence for omitting
GIS SQL function sources and linking a small empty registry/type-constructor
shim at 463,440 bytes from a stripped linked runtime proxy and 864,782 bytes
from the embedded archive. This slice will remeasure the current branch because
the current profile has already trimmed LOAD execution, host-file SQL I/O,
server utility functions, Oracle SQL mode parsing, and XML SQL functions.

Current branch measurement on 2026-05-15 records:

- default embedded archive: 30,253,824 bytes / 28.85 MiB, 689 members, down
  458,824 bytes and 2 members from the XML-trim baseline;
- storage-smoke embedded archive: 30,434,408 bytes / 29.02 MiB, 692 members,
  down 458,824 bytes and 2 members from the XML-trim baseline;
- representative embedded linked smoke binaries are down about 260,800 bytes
  unstripped and about 222,600 bytes stripped, with 400 fewer global symbols.

Both embedded archives omit `item_geofunc.cc.o`, `gcalc_tools.cc.o`, and
`gcalc_slicescan.cc.o`, while still retaining `sql_type_geom.cc.o` and
`spatial.cc.o`.

## License Or Dependency Impact

No new dependencies and no license change.

## Test And Verification Plan

- Add direct execution policy tests for representative GIS SQL functions,
  including at least `ST_AsText()`, `ST_GeomFromText()`, `ST_Contains()`,
  `PointFromText()`, `Point()`, and `X()`.
- Add prepared-statement policy coverage for representative GIS SQL functions.
- Verify quoted mentions of GIS function names still execute as ordinary
  strings.
- Keep SPATIAL-index rejection coverage passing in the storage-smoke suite.
- Reconfigure and rebuild the default embedded MariaDB archive and the
  storage-smoke MariaDB archive.
- Verify embedded archives omit `item_geofunc.cc.o`, `gcalc_tools.cc.o`, and
  `gcalc_slicescan.cc.o`, while still retaining `sql_type_geom.cc.o` and
  `spatial.cc.o`.
- Build and run embedded and storage-smoke presets.
- Run `tools/mylite-compat-harness report server-surface unsupported-index`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, normal MariaDB `sql` target, and
  first-party tidy checks.

## Acceptance Criteria

- The default MyLite embedded profile records
  `MYLITE_WITH_GIS_SQL_FUNCTIONS=OFF`.
- Embedded archives omit GIS SQL function and `gcalc_*` objects.
- Public direct and prepared entry points reject representative GIS SQL
  functions with stable MyLite diagnostics.
- SPATIAL-index and existing geometry DDL rejection coverage still pass.
- Size documentation records the current measured impact.

## Risks And Open Questions

- GIS functions are a larger SQL compatibility surface than XML helpers. The
  risk is acceptable only while geometry storage and SPATIAL access paths remain
  unsupported in the default profile.
- The slice intentionally leaves geometry type code linked, so it is not a full
  GIS removal. Follow-up geometry type or spatial-core trims are separate, more
  compatibility-sensitive decisions.
- The policy scanner duplicates MariaDB registry names in first-party code. That
  is acceptable for an unsupported-surface gate, but a future generated or
  parser-backed policy may be preferable if the list grows.
