# MyISAM Maintenance Trim

## Problem

The embedded profile still carries native MyISAM table maintenance, repair,
key-cache assignment, preload, and auto-repair code. MyLite routes
application-requested `ENGINE=MyISAM` tables to MyLite storage rather than to
durable `.MYD` / `.MYI` files, and MyLite's public SQL policy does not expose
server-style table-maintenance administration as core embedded behavior.

Keeping the inherited MyISAM repair stack preserves disk-file administration
code, parallel repair helpers, key-cache preloading, and index-file repair roots
inside the file-owned embedded profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/myisam/CMakeLists.txt` builds `mi_check.c`,
  `mi_keycache.c`, and `mi_preload.c` into the mandatory MyISAM plugin.
- `mariadb/storage/myisam/mi_check.c` exports `myisamchk_init()`,
  `chk_status()`, `chk_size()`, `chk_del()`, `chk_key()`, `chk_data_link()`,
  `mi_repair()`, `mi_repair_by_sort()`, `mi_repair_parallel()`,
  `mi_sort_index()`, and `update_state_info()`.
- `mariadb/storage/myisam/mi_keycache.c` exports
  `mi_assign_to_key_cache()`, and `mariadb/storage/myisam/mi_preload.c`
  exports `mi_preload()`.
- `mariadb/storage/myisam/ha_myisam.cc` calls those routines from
  `ha_myisam::check()`, `analyze()`, `repair()`, `optimize()`,
  `assign_to_keycache()`, `preload_keys()`, `enable_indexes()` for persistent
  index rebuilds, `start_bulk_insert()` for persistent index disable/rebuild
  decisions, and `check_and_repair()`.
- `mariadb/sql/handler.cc` calls `mi_change_key_cache()` when server key-cache
  variables move tables between key caches.
- `mariadb/sql/sql_admin.cc` routes `CHECK TABLE`, `ANALYZE TABLE`,
  `OPTIMIZE TABLE`, and `REPAIR TABLE` through `mysql_admin_table()` and the
  handler admin methods. It routes `CACHE INDEX` and `LOAD INDEX INTO CACHE`
  through `mysql_assign_to_keycache()` and `mysql_preload_keys()`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_ASSIGN_TO_KEYCACHE` and
  `SQLCOM_PRELOAD_KEYS` as server administration commands.
- Current public MyLite SQL policy rejects many server-oriented surfaces before
  MariaDB execution, but it does not yet reject table-maintenance or key-cache
  administration SQL explicitly.
- Historical branch-level bundle-size research measured the comparable
  candidate as 64,184 stripped linked-smoke bytes and 116,606 archive bytes
  saved while passing smokes and harness. The current profile must be
  remeasured because the trim stack has changed.

## Design

- Add a `MYLITE_WITH_MYISAM_MAINTENANCE` build option that defaults to `ON`
  for normal MariaDB builds and is forced `OFF` by the MyLite embedded baseline.
- When the option is `OFF`, remove `mi_check.c`, `mi_keycache.c`, and
  `mi_preload.c` from the MyISAM plugin source list.
- Compile the native MyISAM maintenance methods to unsupported no-op results in
  the disabled embedded profile:
  - `check()`, `analyze()`, `repair()`, `optimize()`,
    `assign_to_keycache()`, and `preload_keys()` return
    `HA_ADMIN_NOT_IMPLEMENTED`.
  - `check_and_repair()` returns failure without attempting repair.
  - Persistent `enable_indexes()` repair rebuilding returns
    `HA_ERR_WRONG_COMMAND` instead of calling the repair stack.
  - Persistent MyISAM bulk-insert index disabling is skipped so ordinary bulk
    buffering does not depend on later repair-stack index rebuilds.
  - `auto_repair()` returns false in the disabled profile.
- Keep ordinary MyISAM row/index operations available for inherited internal
  uses that still require the mandatory MyISAM plugin.
- Reject direct and prepared MyLite SQL for table-maintenance and key-cache
  administration before MariaDB execution, with stable MyLite diagnostics.
- Keep `ANALYZE TABLE` unsupported for now. It shares the MyISAM check stack
  and persistent engine statistics path, and MyLite does not yet have
  catalog-backed statistics.

## Affected Subsystems

- MyISAM storage-engine build and handler admin methods.
- Public SQL policy and server-surface compatibility coverage.
- Embedded archive measurement and size-profile documentation.

## MySQL/MariaDB Compatibility Impact

Native MySQL/MariaDB table-maintenance SQL is outside the current MyLite core
library contract. Applications can still use ordinary routed DDL/DML for
`ENGINE=MyISAM` tables; `CHECK TABLE`, `ANALYZE TABLE`, `OPTIMIZE TABLE`,
`REPAIR TABLE`, their representative `LOCAL` / `NO_WRITE_TO_BINLOG` forms,
`CACHE INDEX`, and `LOAD INDEX INTO CACHE` become explicitly unsupported
through the public API.

## Single-File And Embedded-Lifecycle Impact

The disabled profile must not introduce MyISAM repair temporary files, backup
files, persistent key-cache assignment side effects, or MyISAM auto-repair file
mutations. Existing embedded open/close and storage-smoke sidecar gates must
continue to pass.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Unsupported table-maintenance
SQL should fail with a stable `MYLITE_ERROR` diagnostic before MariaDB
execution.

## Storage-Engine Routing Impact

No change to routed `ENGINE=MyISAM` application table DDL/DML. The native
MyISAM maintenance implementation remains unavailable in the disabled embedded
profile because routed MyISAM table state is stored in MyLite storage, not in
native `.MYD` / `.MYI` files.

## Binary-Size Impact

Measured on 2026-05-15 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,519,504 bytes / 26.24 MiB | 676 | -87,712 bytes, -3 members |
| Storage-smoke | 27,700,088 bytes / 26.42 MiB | 679 | -87,704 bytes, -3 members |

The disabled default and storage-smoke MyISAM archives omit `mi_check.c.o`,
`mi_keycache.c.o`, and `mi_preload.c.o` in both normal and embedded MyISAM
archive variants.

## Implementation Notes

- `MYLITE_WITH_MYISAM_MAINTENANCE` defaults to `ON` for upstream-compatible
  builds and is forced `OFF` by `cmake/mariadb-embedded-baseline.cmake`.
- The option is defined in the MyISAM, SQL, and embedded SQL build roots so the
  handler, key-cache, and archive-source guards use the same profile switch.
- Direct and prepared SQL policy rejects table-maintenance and key-cache
  administration, including representative `LOCAL` and `NO_WRITE_TO_BINLOG`
  forms, before MariaDB execution with a stable `table-maintenance` diagnostic.
- Ordinary routed `ENGINE=MyISAM` application-table DDL/DML remains covered
  through the storage-smoke profile. The disabled profile skips only native
  MyISAM table-file maintenance, key-cache assignment/preload, auto-repair, and
  repair-stack index rebuild paths.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived source objects from the
disabled embedded profile only and keeps normal MariaDB defaults intact.

## Test And Verification Plan

- Add direct and prepared SQL policy tests for `CHECK TABLE`, `ANALYZE TABLE`,
  `OPTIMIZE TABLE`, `REPAIR TABLE`, `CACHE INDEX`, and `LOAD INDEX INTO CACHE`.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm disabled embedded MyISAM archives omit `mi_check.c.o`,
  `mi_keycache.c.o`, and `mi_preload.c.o`.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Verification Results

The implementation was verified with:

- `tools/mariadb-embedded-build configure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `cmake --preset embedded-dev`
- `cmake --preset storage-smoke-dev`
- `cmake --build --preset embedded-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev --output-on-failure`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`

## Acceptance Criteria

- Public direct and prepared SQL reject table-maintenance and key-cache
  administration before MariaDB execution.
- Default embedded and storage-smoke archives omit the disabled MyISAM
  maintenance objects and have recorded size reductions.
- Supported routed `ENGINE=MyISAM` DDL/DML, application-schema smoke, sidecar
  gates, embedded lifecycle, and server-surface coverage still pass.
- Normal MariaDB builds keep the default MyISAM maintenance implementation.
- Documentation records the exact unsupported boundary and measurements.

## Risks And Open Questions

- Native MyISAM still serves inherited internal engine paths until later
  temp-spill and mandatory-engine work removes or narrows it further.
- MyISAM bulk-insert index re-enable paths share the repair stack. The disabled
  profile should preserve existing MyLite storage-smoke coverage, but broader
  upstream MyISAM bulk-load behavior is intentionally unsupported.
- `ANALYZE TABLE` may become useful later once MyLite has catalog-backed
  statistics. For now it is grouped with table-maintenance administration
  because the native implementation uses the removable check stack.
