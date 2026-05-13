# No-MyISAM Temp-Spill Size Profile

## Problem

The aggressive MyLite minsize profile hides user-created `ENGINE=MyISAM`, but
previously still linked MariaDB's MyISAM engine because the inherited SQL layer
uses MyISAM as the disk temporary-table engine when Aria temporary tables are
disabled.

Current measured MyISAM component:

| Artifact | Bytes | Objects |
| --- | ---: | ---: |
| `storage/myisam/libmyisam_embedded.a` | 703,372 | 54 |

The linked open/close smoke still contains live MyISAM symbols such as
`mi_open`, `mi_create`, `mi_repair`, `mi_repair_by_sort`, `chk_key`, and
`chk_data_link`.

## Source Findings

MariaDB source references are from imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/storage/myisam/CMakeLists.txt` registers MyISAM with
  `MYSQL_ADD_PLUGIN(... STORAGE_ENGINE MANDATORY RECOMPILE_FOR_EMBEDDED)`.
  The previous legacy-engine size slice therefore could not disable MyISAM with
  only `PLUGIN_MYISAM=NO`.
- `vendor/mariadb/server/storage/myisam/ha_myisam.cc` marks MyISAM with
  `HTON_NOT_USER_SELECTABLE` under `MYLITE_DISABLE_LEGACY_STORAGE_ENGINES`, so
  explicit user `ENGINE=MyISAM` is already blocked.
- `vendor/mariadb/server/sql/sql_class.h` defines the inherited disk temporary
  table engine as `TMP_ENGINE_HTON myisam_hton` and
  `TMP_ENGINE_COLUMNDEF MI_COLUMNDEF` when `USE_ARIA_FOR_TMP_TABLES` is off.
- `vendor/mariadb/server/sql/sql_select.cc:22257` chooses `TMP_ENGINE_HTON`
  for temporary tables with BLOB fields, `BIG_TABLES`, explicit
  `TMP_TABLE_FORCE_MYISAM`, or zero `tmp_memory_table_size`.
- `vendor/mariadb/server/sql/sql_select.cc:23468` converts full HEAP temporary
  tables to `TMP_ENGINE_HTON` in `create_internal_tmp_table_from_heap()`.
- `vendor/mariadb/server/sql/keycaches.cc:349` copies optimizer costs from
  `TMP_ENGINE_HTON`.
- `vendor/mariadb/server/sql/mysqld.cc:9542` exposes the stage text
  `"Converting HEAP to MyISAM"` through `TMP_ENGINE_NAME`.

## Scope

This slice makes `MYLITE_DISABLE_MYISAM_TEMP_SPILL` viable in the default
aggressive minsize profile. The option:

- prevents `storage/myisam` from registering the mandatory MyISAM plugin,
- keeps user `ENGINE=MyISAM` rejected through the existing legacy-engine
  smoke checks,
- rejects inherited disk temporary-table spill paths with an explicit MariaDB
  diagnostic instead of silently linking MyISAM,
- bounds built-in schema-table long-text columns to MEMORY-compatible `VARCHAR`
  columns in this aggressive profile,
- keeps in-memory `HEAP`/`MEMORY` temporary tables available, and
- measures the archive and stripped linked impact.

## Non-Goals

This slice does not implement a replacement MyLite-owned disk temporary-table
engine. It is an aggressive size experiment, not the final answer for large
sorts, BLOB temporary tables, or memory-exhausted temporary tables.

This slice does not remove `HEAP`/`MEMORY`. The SQL layer uses it for normal
temporary table execution, and it is not durable sidecar storage.

## Proposed Design

Add `MYLITE_DISABLE_MYISAM_TEMP_SPILL` as a minsize build switch and enable it
by default in `tools/build-mariadb-minsize.sh`.

In `storage/myisam/CMakeLists.txt`, return before `MYSQL_ADD_PLUGIN(myisam ...)`
when both `MYLITE_DISABLE_LEGACY_STORAGE_ENGINES` and
`MYLITE_DISABLE_MYISAM_TEMP_SPILL` are enabled.

In the SQL layer, keep compile-time `MI_COLUMNDEF` types available where the
existing temporary-table function signatures require them, but guard runtime
paths that would instantiate or dereference `TMP_ENGINE_HTON`:

- `Create_tmp_table::choose_engine()` should fail with `ER_NOT_SUPPORTED_YET`
  for disk temporary tables in this profile.
- `create_internal_tmp_table_from_heap()` should fail with
  `ER_NOT_SUPPORTED_YET` when a HEAP table would spill to disk.
- `copy_tmptable_optimizer_costs()` should copy HEAP costs into
  `tmp_table_optimizer_costs` instead of dereferencing `TMP_ENGINE_HTON`.

The implementation is conservative: reject disk-spill paths before the MyISAM
handlerton is needed. Built-in schema-table `Longtext` fields are bounded to
the largest portable `VARCHAR` size in this profile so common metadata queries
remain MEMORY-temporary-table compatible.

## Affected Subsystems

- Storage plugin registration and embedded static plugin list.
- Internal temporary table engine selection.
- HEAP-to-disk temporary-table conversion.
- Optimizer temporary-table cost initialization.
- Compatibility behavior for large result sets, BLOB temp tables, and forced
  disk temporary tables.

## Single-File and Embedded-Lifecycle Impact

Removing MyISAM moves the aggressive profile closer to MyLite's single-file
shape because no inherited `.MYD`/`.MYI` table files can be produced by MyISAM.
The tradeoff is that disk temporary-table spill is unavailable until MyLite
implements a MyLite-owned replacement.

## Public API and File-Format Impact

No public `libmylite` API or file-format change.

SQL behavior changes only in the aggressive minsize profile: queries that
require disk temporary tables may fail instead of spilling through MyISAM.

## Binary-Size Impact

The component archive is about 0.67 MiB after current no-binlog-core work. The
linked runtime impact is expected to be smaller because section GC already drops
unreferenced MyISAM functions, but the current linked smoke still retains MyISAM
open, create, check, and repair symbols.

Measured with:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-myisam-temp-spill-current \
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
```

On top of `select-outfile-size-profile`:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,994,786 | -419,960 |
| archive object count | 370 | -41 |
| unstripped `mylite-open-close-smoke` | 6,696,024 | -151,368 |
| stripped `mylite-open-close-smoke` | 4,708,544 | -117,152 |

`storage/myisam/libmyisam_embedded.a` is not produced in the experimental
build, and the merged archive contains no `ha_myisam.cc.o`, `mi_*.c.o`,
`ft_*.c.o`, or `rt_*.c.o` members. The linked smoke still has inherited
top-level globals such as `myisam_hton` and `THR_LOCK_myisam`, but no live
`ha_myisam` or `mi_*` implementation symbols.

## Test and Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-myisam-temp-spill-current MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-myisam-temp-spill-current MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-myisam-temp-spill-current MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
```

Add smoke coverage for at least one query that still uses in-memory temporary
tables successfully, and one query that requires disk temporary-table spill and
fails explicitly in the aggressive profile.

Measure:

- `libmysqld/libmariadbd.a` bytes and object count,
- stripped `mylite-open-close-smoke` bytes,
- whether `builtin_maria_myisam_plugin` is absent from the archive,
- whether `ha_myisam.cc.o` and `mi_*.c.o` objects are absent, and
- retained MyISAM symbols in the linked smoke.

## Acceptance Criteria

- The aggressive build links without the MyISAM plugin or MyISAM component
  objects.
- Existing MyLite open/close smokes pass and include explicit coverage for
  in-memory temp tables plus disk-temp-spill rejection.
- The compatibility harness passes.
- User `ENGINE=MyISAM` remains explicitly unavailable.
- In-memory temporary-table queries still execute.
- Size deltas and any rejected removal roots are recorded in
  `docs/research/production-size-analysis.md`.

## Verification Result

The default minsize build, `tools/run-libmylite-open-close-smoke.sh`, and
`tools/run-compatibility-test-harness.sh` pass. The open/close smoke verifies a
small in-memory temporary-table query succeeds and that forced disk
temporary-table selection fails with `ER_NOT_SUPPORTED_YET` and message text
`MyISAM temporary table spill`.

The initial opt-in version failed in `storage_single_file`: schema-table
metadata and several smoke-report aggregation queries routed through MariaDB's
inherited disk temporary-table path. The final version keeps common schema
metadata in MEMORY by bounding built-in schema-table long-text columns, and the
storage smoke now joins ordered multi-row assertions in the client where the
test is checking MyLite storage behavior rather than ordered aggregate
temporary-table behavior.

## Risks and Unresolved Questions

- This removes a real MariaDB execution fallback. Queries with BLOB temp
  columns, forced disk temporary tables, or exhausted HEAP limits may now fail.
- Several SQL paths use `TMP_ENGINE_HTON` directly. Missing one could become a
  null-handlerton crash if MyISAM is omitted.
- A production default may need a MyLite-owned disk-spill engine instead of
  accepting these failures.
