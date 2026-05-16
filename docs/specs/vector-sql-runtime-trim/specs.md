# Vector SQL Runtime Trim

## Problem

MariaDB 11.8 includes vector SQL functions and the built-in internal `mhnsw`
plugin for vector indexes. MyLite already treats vector indexes as an
unsupported key class in storage-design documents, and the current storage
engine only supports ordinary BTREE-like primary, unique, secondary, and
bounded prefix indexes.

The default embedded archive still carries `item_vectorfunc.cc` and
`vector_mhnsw.cc`. That keeps vector conversion, vector distance, HNSW graph
maintenance, transaction participant, and mandatory plugin registration code in
the embedded profile even though MyLite cannot persist or query vector indexes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation describes vectors as available from MariaDB 11.7
and names `VECTOR(N)`, `VECTOR INDEX`, `VEC_FromText()`,
`VEC_ToText()`, `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and
`VEC_DISTANCE_COSINE()` as the public vector surface:

- <https://mariadb.com/docs/server/reference/sql-structure/vectors/vector-overview>
- <https://mariadb.com/docs/server/reference/plugins/other-plugins/mhnsw>

Relevant source paths:

- `mariadb/sql/item_vectorfunc.h` declares the vector SQL item classes:
  `Item_func_vec_distance`, `Item_func_vec_totext`, and
  `Item_func_vec_fromtext`.
- `mariadb/sql/item_vectorfunc.cc` implements vector distance selection,
  distance calculation, binary-to-text conversion, text-to-binary parsing, and
  validation warnings.
- `mariadb/sql/item_create.cc` includes `item_vectorfunc.h`, defines
  `Create_func_vec_*` builders, and registers `VEC_DISTANCE_EUCLIDEAN`,
  `VEC_DISTANCE_COSINE`, `VEC_DISTANCE`, `VEC_FROMTEXT`, and `VEC_TOTEXT`.
- `mariadb/sql/vector_mhnsw.h` declares the current internal vector-index API
  used by table, DDL, optimizer, and SHOW paths.
- `mariadb/sql/vector_mhnsw.cc` implements HNSW graph cache structures, graph
  insert/search/update/delete, vector index table definition helpers, index
  options, a transaction participant, system variables, and the built-in
  `mhnsw` plugin declaration.
- `mariadb/sql/sql_builtin.cc.in` hardcodes `builtin_maria_mhnsw_plugin` in the
  built-in plugin extern list and `mysql_mandatory_plugins[]`.
- `mariadb/sql/sql_table.cc` maps `Key::VECTOR` to `HA_KEY_ALG_VECTOR`, uses
  `mhnsw_plugin` and `mhnsw_index_options`, and compares vector index options
  during ALTER.
- `mariadb/sql/sql_base.cc`, `mariadb/sql/table.cc`, and
  `mariadb/sql/sql_show.cc` call `mhnsw_*` helpers for retained table-share,
  HNSW lifecycle, and metadata formatting paths.
- `mariadb/storage/mylite/ha_mylite.cc` rejects keys whose algorithm is neither
  `HA_KEY_ALG_UNDEF` nor `HA_KEY_ALG_BTREE`, so `HA_KEY_ALG_VECTOR` is outside
  the supported MyLite key shape.
- The current default embedded archive contains `item_vectorfunc.cc.o` and
  `vector_mhnsw.cc.o`.

## Design

- Add `MYLITE_WITH_VECTOR_SQL_RUNTIME`, defaulting to `ON` for upstream-like
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, remove `item_vectorfunc.cc` and `vector_mhnsw.cc`
  from the SQL and embedded SQL source lists and compile one MyLite-owned
  disabled source instead.
- Guard the vector native-function builders and registry rows in
  `item_create.cc` behind `MYLITE_WITH_VECTOR_SQL_RUNTIME`.
- Guard hardcoded `builtin_maria_mhnsw_plugin` extern and mandatory-plugin
  registration in `sql_builtin.cc.in` behind the same option.
- Keep `item_vectorfunc.h` and `vector_mhnsw.h` declarations available because
  retained source files still include them and still refer to the enum and
  `mhnsw_*` signatures.
- In the disabled source, define link-compatible `mhnsw_*` stubs:
  - mutating and read functions fail closed with `HA_ERR_UNSUPPORTED`;
  - free is a no-op;
  - option and plugin globals remain defined;
  - distance selection returns a stable default only for retained metadata
    paths that still need a return value.
- Reject public direct and prepared MyLite SQL calls to the `VEC_*` vector
  functions before MariaDB execution with a stable unsupported-surface
  diagnostic.
- Keep the `VECTOR(N)` SQL type code in this slice. Removing the type handler is
  a separate compatibility decision because it changes DDL parsing and column
  metadata behavior more broadly than removing function/index runtime.

## Affected Subsystems

- MariaDB SQL and embedded SQL build source selection.
- Generated built-in plugin registration.
- Native SQL function registry.
- Retained table, DDL, optimizer, and metadata paths that reference `mhnsw_*`.
- Public direct/prepared SQL policy and compatibility tests.
- Embedded archive and linked-runtime size reporting.

## MySQL/MariaDB Compatibility Impact

MariaDB Server supports vector search. MyLite's current default embedded
profile does not. Direct and prepared calls to `VEC_FROMTEXT()`,
`VEC_TOTEXT()`, `VEC_DISTANCE()`, `VEC_DISTANCE_EUCLIDEAN()`, and
`VEC_DISTANCE_COSINE()` become explicitly unsupported in the core profile.

This is a compatibility tradeoff. It is acceptable for the default profile
while MyLite lacks vector storage, vector index persistence, HNSW graph
maintenance in the `.mylite` file, optimizer coverage, and recovery semantics.

## DDL Metadata Routing Impact

Vector index DDL must not publish MyLite catalog metadata as though vector
access paths were supported. The existing MyLite key-shape gate rejects
`HA_KEY_ALG_VECTOR`; this slice should add explicit coverage so a disabled
`mhnsw` plugin does not turn vector indexes into accidental support.

## Single-File And Embedded-Lifecycle Impact

No public file-format change and no new companion files. The disabled profile
does not create HNSW graph tables, cache files, transaction participants, or
plugin state.

## Public API And File-Format Impact

No C API or `.mylite` file-format change. Direct execution and prepared
statement preparation fail with `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB
errno, and a diagnostic naming the unsupported vector SQL function surface.

## Storage-Engine Routing Impact

No supported table-storage behavior changes. The current MyLite storage engine
continues to reject vector indexes as unsupported key shapes. Ordinary supported
BTREE-like keys must continue to pass.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol adapters must not infer vector support from retained
`VECTOR(N)` parsing or disabled `mhnsw_*` stubs. Vector search needs a separate
compatibility profile or storage implementation.

## Binary-Size Impact

The archived bundle-size research ranks omitting vector SQL functions and
MHNSW sources as a small linked-runtime win and a larger archive cleanup. The
historical branch measured about 230 KiB of archive reduction, but this slice
must remeasure against the current profile after recent server-surface trims.

## License And Dependency Impact

No new dependency and no license change. The disabled source is first-party
GPL-2.0-compatible code inside the MariaDB-derived tree. The option removes
MariaDB-derived vector runtime objects only from the disabled embedded profile.

## Test And Verification Plan

- Add direct SQL policy coverage for representative vector functions:
  `VEC_FROMTEXT()`, `VEC_TOTEXT()`, `VEC_DISTANCE()`,
  `VEC_DISTANCE_EUCLIDEAN()`, and `VEC_DISTANCE_COSINE()`.
- Add prepared statement policy coverage for representative vector functions.
- Verify quoted mentions of vector function names still execute as ordinary
  strings.
- Add storage-smoke coverage that routed `VECTOR KEY` DDL fails before MyLite
  catalog publication and leaves existing table counts unchanged.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both disabled embedded archives omit `item_vectorfunc.cc.o` and
  `vector_mhnsw.cc.o` and include the MyLite disabled stub.
- Run embedded and storage-smoke presets.
- Run the `server-surface` and `unsupported-index` compatibility reports.
- Run the first-party size report.
- Run `dev`, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- `MYLITE_WITH_VECTOR_SQL_RUNTIME=OFF` is recorded in the default embedded
  baseline.
- The default and storage-smoke embedded archives omit vector SQL runtime and
  MHNSW runtime objects.
- Public direct and prepared SQL reject representative vector SQL functions
  before MariaDB execution.
- Routed vector-index DDL remains rejected before catalog publication.
- Docs record the unsupported boundary and measured current size impact.

## Implementation Result

- Added `MYLITE_WITH_VECTOR_SQL_RUNTIME`, defaulting to `ON` outside MyLite's
  embedded baseline and forced `OFF` in the default embedded profile.
- The disabled profile replaces `item_vectorfunc.cc` and `vector_mhnsw.cc` with
  `mylite_vector_sql_runtime_disabled.cc` in both SQL and embedded SQL source
  lists.
- `item_create.cc` no longer registers `VEC_*` native functions when the option
  is disabled, and `sql_builtin.cc.in` no longer registers the mandatory
  `mhnsw` plugin in that profile.
- Public direct and prepared MyLite entry points reject representative `VEC_*`
  calls before MariaDB execution while quoted mentions still execute as string
  literals.
- Storage-smoke coverage now rejects routed `VECTOR KEY` DDL before catalog
  publication and verifies the table count is unchanged.

## Measured Binary-Size Impact

Measured on 2026-05-16 from the same host and toolchain as the previous
embedded profile baseline:

| Profile | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Default embedded archive | 26,610,000 bytes / 25.38 MiB, 670 members | 26,513,480 bytes / 25.29 MiB, 669 members | -96,520 bytes, -1 member |
| Storage-smoke archive | 26,805,392 bytes / 25.56 MiB, 673 members | 26,708,872 bytes / 25.47 MiB, 672 members | -96,520 bytes, -1 member |

The first-party linked smoke binaries also dropped by roughly 22-55 KiB
stripped, depending on the executable, and removed 58 global symbols from the
representative linked targets.

## Verification Results

- `tools/mariadb-embedded-build configure && tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- Archive membership checks for default and storage-smoke profiles: only
  `mylite_vector_sql_runtime_disabled.cc.o` remains; `item_vectorfunc.cc.o` and
  `vector_mhnsw.cc.o` are absent.
- `nm -C build/mariadb-embedded/libmysqld/libsql_embedded.a` check for
  `Item_func_vec`, `Create_func_vec`, `MHNSW_Share`, and
  `builtin_maria_mhnsw`: no matches.
- `cmake --preset embedded-dev && cmake --build --preset embedded-dev && ctest --preset embedded-dev --output-on-failure`
- `cmake --preset storage-smoke-dev && cmake --build --preset storage-smoke-dev && ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface unsupported-index`
- `tools/mylite-size-report`

## Risks And Open Questions

- `VECTOR(N)` type parsing remains present. A later `VECTOR` type-handler trim
  may be reasonable, but it should be separate because it changes column type
  compatibility beyond function/index runtime.
- Retained MariaDB sources still include `vector_mhnsw.h`. Future upstream
  changes may add more `mhnsw_*` references; link failures and archive symbol
  checks should catch that during rebase work.
- Applications using MariaDB vector search will need either a fuller MyLite
  vector storage design or an alternate profile with this runtime enabled.
