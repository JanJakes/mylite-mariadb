# gis-function-size-profile

## Problem

The minsize profile still links MariaDB's native GIS function registry and GIS
calculation helpers even though MyLite storage deliberately rejects GEOMETRY
columns and SPATIAL indexes today. This keeps a broad set of geometry
constructors, predicates, and measurements in the embedded binary without a
current product path to store or index those values.

This slice tests whether MyLite can omit GIS SQL functions from the default
minsize embedded profile while preserving geometry type parsing and existing
GEOMETRY/SPATIAL DDL rejection coverage.

## Source baseline

- MariaDB Server tag `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current MyLite size baseline after `xml-function-size-profile`:
  - `libmariadbd.a`: 33,957,690 bytes,
  - stripped `mylite-open-close-smoke`: 15,585,480 bytes,
  - linked `size` total: 15,864,099 bytes.

## Source findings

- `vendor/mariadb/server/libmysqld/CMakeLists.txt` includes
  `../sql/item_geofunc.cc`, `../sql/gcalc_tools.cc`, and
  `../sql/gcalc_slicescan.cc` in `SQL_EMBEDDED_SOURCES`.
- `vendor/mariadb/server/sql/item_geofunc.cc` implements the native GIS
  function builders and exports `native_func_registry_array_geom`.
- `vendor/mariadb/server/sql/item_create.cc` appends
  `native_func_registry_array_geom` into both normal and Oracle native function
  hashes during `item_create_init()`.
- `vendor/mariadb/server/sql/gcalc_tools.cc` and
  `vendor/mariadb/server/sql/gcalc_slicescan.cc` provide geometry operation
  machinery used by GIS functions such as buffer, intersection, union, and
  difference.
- `vendor/mariadb/server/sql/sql_type_geom.cc` and
  `vendor/mariadb/server/sql/spatial.cc` implement geometry type and WKB/WKT
  support that is still needed for parser/type recognition and current MyLite
  GEOMETRY rejection tests. They are not removed in this slice.
- `vendor/mariadb/server/storage/mylite/ha_mylite.cc` rejects
  `MYSQL_TYPE_GEOMETRY` columns and `HA_SPATIAL_legacy` keys.
- `vendor/mariadb/server/mylite/storage_engine_smoke.cc` already verifies
  unsupported GEOMETRY table and SPATIAL key DDL are rejected.
- Current object-section sizes:
  - `item_geofunc.cc.o`: 171,696 bytes,
  - `gcalc_tools.cc.o`: 11,058 bytes,
  - `gcalc_slicescan.cc.o`: 10,976 bytes.
- Current unstripped object-file sizes:
  - `item_geofunc.cc.o`: 908,920 bytes,
  - `gcalc_tools.cc.o`: 30,464 bytes,
  - `gcalc_slicescan.cc.o`: 25,184 bytes.
- MariaDB's official ST_AsText documentation describes one representative GIS
  function as converting an internal geometry value to WKT:
  - <https://mariadb.com/kb/en/st_astext/>

## Proposed design

Add an embedded-library CMake option:

```cmake
MYLITE_DISABLE_GIS_FUNCTIONS
```

When enabled:

- remove `../sql/item_geofunc.cc`, `../sql/gcalc_tools.cc`, and
  `../sql/gcalc_slicescan.cc` from `SQL_EMBEDDED_SOURCES`,
- add a small embedded-only stub source that defines an empty
  `Native_func_registry_array native_func_registry_array_geom` and the
  retained geometry type link shims required by `sql_type_geom.cc` and
  `spatial.cc`,
- set `-DMYLITE_DISABLE_GIS_FUNCTIONS=ON` in `tools/build-mariadb-minsize.sh`.

The stub keeps `item_create.cc` unchanged: the normal native-function registry
still appends the geometry registry, but the registry has zero entries in the
minsize profile. The retained type-constructor shims fail if executed; the
retained `Gcalc_result_receiver` result-type shim is only for link completeness
because GIS operations are not registered in this profile.

## Non-goals

- Do not remove `GEOMETRY` type parsing.
- Do not remove `sql_type_geom.cc` or `spatial.cc`.
- Do not remove SPATIAL index syntax.
- Do not change MyLite's current GEOMETRY and SPATIAL DDL rejection behavior.
- Do not change the full MariaDB server target.
- Do not add a public API change.

## Affected subsystems

- Embedded library build graph.
- Native SQL function registration.
- `libmylite` open/close smoke coverage for unsupported minsize SQL surfaces.
- Existing storage-engine smoke coverage for GEOMETRY/SPATIAL DDL rejections.

## DDL metadata routing impact

Geometry columns and SPATIAL keys remain rejected by the MyLite storage engine.
This slice must not let unsupported table definitions persist, and it must not
create new `.frm` or schema side effects.

## Single-file and embedded-lifecycle impact

No file-format, catalog, lock, recovery, or lifecycle changes are expected. The
grouped compatibility harness must continue to prove the existing GEOMETRY and
SPATIAL DDL rejection paths and sidecar scan.

## Public API and file-format impact

No public `libmylite` API change. No `.mylite` file-format change.

## Binary-size impact

Expected savings are bounded by the linked parts of `item_geofunc.cc.o` and
the two `gcalc_*` objects. The unstripped object files total about 942 KiB, but
their section totals are about 189 KiB before link/layout overhead. The final
decision must use production artifact measurements.

## License, trademark, and dependency impact

No new dependency or license impact. The change removes MariaDB-derived GIS
function implementation from the default minsize profile but keeps the imported
source in the tree.

## Test plan

Run:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Add open/close smoke assertions that:

- `SELECT ST_ASTEXT(0x00)` fails in the minsize profile,
- the failure uses MariaDB's unknown-function path after the GIS native
  registry is empty.

Keep relying on the storage smoke inside the compatibility harness for:

- `CREATE TABLE ... GEOMETRY ... ENGINE=MYLITE` rejection,
- `CREATE TABLE ... GEOMETRY ..., SPATIAL KEY ... ENGINE=MYLITE` rejection.

Measure:

```sh
stat -c "%s" build/mariadb-minsize/libmysqld/libmariadbd.a
ar t build/mariadb-minsize/libmysqld/libmariadbd.a | wc -l
stat -c "%s" build/mariadb-minsize/mylite/libmylite.a
stat -c "%s" build/mariadb-minsize/mylite/mylite-open-close-smoke
cp build/mariadb-minsize/mylite/mylite-open-close-smoke \
  build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
strip --strip-unneeded \
  build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
stat -c "%s" build/mariadb-minsize/mylite/mylite-open-close-smoke.stripped
size build/mariadb-minsize/mylite/mylite-open-close-smoke
```

## Acceptance criteria

- The minsize build succeeds with `MYLITE_DISABLE_GIS_FUNCTIONS=ON`.
- The linked smoke binary no longer contains `Item_func_geometry_from_text`,
  `Create_func_geometry_from_text`, or `Gcalc_function` symbols.
- Supported open/close, SQL execution, DML, prepared statement, read-only, URI,
  storage, and compatibility smokes pass.
- `ST_ASTEXT()` is rejected in the minsize profile.
- GEOMETRY and SPATIAL DDL rejection tests still pass.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.

## Verification

Validated on 2026-05-12 with:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Observed smoke evidence:

- `exec_gis_function_message=FUNCTION ST_ASTEXT does not exist`
- `unsupported_geometry=rejected`
- `unsupported_spatial_key=rejected`

Measured artifacts after this slice:

| Artifact | Bytes | Delta from XML profile |
| --- | ---: | ---: |
| `libmariadbd.a` | 33,092,908 | -864,782 |
| archive object count | 488 | -2 |
| `libmylite.a` | 93,752 | 0 |
| `libmylite_embedded.a` | 303,480 | 0 |
| `mylite-open-close-smoke` | 17,424,784 | -548,456 |
| stripped `mylite-open-close-smoke` copy | 15,122,040 | -463,440 |
| linked `size` total | 15,373,843 | -490,256 |

Symbol checks on the linked smoke and `libmariadbd.a` found no
`Item_func_geometry_from_text`, `Create_func_geometry_from_text`, or
`Gcalc_function` symbols.

## Risks and unresolved questions

- Removing GIS functions is a significant SQL compatibility tradeoff. It is
  acceptable only for the minsize profile while MyLite storage cannot persist
  geometry values.
- Geometry type code remains linked, so this does not remove every GIS-related
  byte.
- If later MyLite implements GEOMETRY storage, this profile decision must be
  revisited with a compatibility matrix.
