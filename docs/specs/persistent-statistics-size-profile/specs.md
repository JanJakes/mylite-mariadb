# Persistent Statistics Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's persistent
engine-independent statistics implementation in `sql_statistics.cc` and the
JSON histogram implementation in `opt_histogram_json.cc`. These paths read,
write, rename, and delete rows in MariaDB's `mysql.table_stats`,
`mysql.column_stats`, and `mysql.index_stats` system tables.

MyLite's current embedded profile has no MyLite-owned persistent optimizer
statistics catalog. The inherited `ANALYZE TABLE` table-maintenance command is
also unsupported in this profile, so keeping the `mysql.*` statistics-table
runtime is mostly dead server metadata surface.

Current baseline after `table-admin-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,778,266 |
| `sql_statistics.cc.o` object | 111,112 |
| `opt_histogram_json.cc.o` object | 43,192 |
| stripped `mylite-open-close-smoke` | 5,674,104 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/sql/sql_statistics.cc` documents persistent
  statistics as rows stored in `mysql.table_stats`, `mysql.column_stats`, and
  `mysql.index_stats`.
- `vendor/mariadb/server/sql/sql_statistics.h` exposes the optimizer and DDL
  hook surface: `read_statistics_for_tables_if_needed()`,
  `read_statistics_for_tables()`, `set_statistics_for_table()`,
  `delete_statistics_for_table()`, column/index statistics rename/delete
  helpers, `is_eits_usable()`, and selectivity helpers.
- `vendor/mariadb/server/sql/sql_base.cc` calls
  `read_statistics_for_tables_if_needed()` during table open.
- `vendor/mariadb/server/sql/sql_select.cc`, `sql_delete.cc`,
  `sql_update.cc`, and `opt_range.cc` read engine-independent statistics for
  cost estimates when the statistics are usable.
- `vendor/mariadb/server/sql/sql_table.cc`, `sql_alter.cc`,
  `sql_rename.cc`, `sql_db.cc`, and `ddl_log.cc` update or delete persistent
  statistics rows during DDL.
- `vendor/mariadb/server/sql/opt_histogram_json.cc` implements the
  `JSON_HB` histogram format used by persistent statistics.
- Symbol intersection against the current archive shows only 13
  externally-needed symbols when `sql_statistics.cc.o` and
  `opt_histogram_json.cc.o` are omitted together.

## Scope

Add a minsize option that removes inherited persistent statistics storage from
the embedded library. The option will:

- remove `../sql/sql_statistics.cc` and `../sql/opt_histogram_json.cc` from
  `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned persistent-statistics stub;
- make optimizer statistics reads no-ops;
- make statistics-table DDL maintenance hooks no-ops; and
- keep ordinary optimizer planning, DDL, DML, and table open behavior working
  from engine estimates and default selectivity.

## Non-Goals

- Do not implement MyLite-native optimizer statistics or histograms.
- Do not remove optimizer code that consumes handler-provided statistics.
- Do not remove `EXPLAIN FORMAT=JSON` writer support.
- Do not remove general JSON parsing/writing helpers used outside persistent
  statistics.
- Do not change non-embedded MariaDB behavior.

## Proposed Design

Add `MYLITE_DISABLE_PERSISTENT_STATISTICS` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_persistent_statistics_stub.cc`.
The stub will define the retained public hooks from `sql_statistics.h`:

- read hooks return success without loading any `mysql.*` statistics tables;
- DDL maintenance hooks return success without touching statistics tables;
- `set_statistics_for_table()` copies handler row estimates into
  `TABLE::used_stat_records` and clears EITS key flags;
- `is_eits_usable()` returns `false`;
- column selectivity helpers return handler table cardinality fallbacks; and
- `TABLE_STATISTICS_CB::~TABLE_STATISTICS_CB()` is a no-op because the stub
  never allocates statistics payloads.

## Affected Subsystems

- Embedded minsize SQL source list.
- Table-open statistics read hook.
- Optimizer selectivity helpers for engine-independent statistics.
- DDL hooks for persistent statistics row maintenance.
- Binary-size documentation.

## DDL Metadata Routing Impact

This removes inherited DDL maintenance of `mysql.*` statistics-table rows.
MyLite table definitions and rows continue to live in the MyLite catalog; this
slice does not change table DDL routing or file-format metadata.

## Single-File And Embedded-Lifecycle Impact

The slice removes a server system-table metadata path that does not belong to
the `.mylite` primary file today. It does not add companion files and does not
change locking or recovery behavior.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are bounded by the 154,304 bytes from
`sql_statistics.cc.o` plus `opt_histogram_json.cc.o` minus the replacement
stub. Linked-runtime savings may be smaller because many optimizer callers stay
live and only the persistent-statistics branches disappear.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `sql_statistics.cc.o` and `opt_histogram_json.cc.o` in
  `libmariadbd.a`;
- presence and size of the replacement stub; and
- compatibility-harness status.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- The embedded archive no longer contains `sql_statistics.cc.o` or
  `opt_histogram_json.cc.o`.
- Ordinary SELECT, INSERT, UPDATE, DELETE, DDL, and MyLite sidecar checks still
  pass through the compatibility harness.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- This removes MariaDB's persistent engine-independent optimizer statistics
  from the aggressive profile. Query plans may differ for workloads that depend
  on `mysql.*` statistics tables.
- Future MyLite-native `ANALYZE`, statistics persistence, or histogram support
  needs a storage design in the `.mylite` catalog rather than resurrecting the
  inherited `mysql.*` tables.
- `use_stat_tables` remains a visible MariaDB system variable for now, but its
  read path is inert in the aggressive profile.

## Implementation Result

Implemented with `MYLITE_DISABLE_PERSISTENT_STATISTICS=ON` in the minsize
profile. The embedded archive now omits `sql_statistics.cc.o` and
`opt_histogram_json.cc.o` and retains only
`mylite_persistent_statistics_stub.cc.o`.

| Artifact | Bytes | Delta from table-admin profile |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,621,550 | -156,716 |
| `mylite/libmylite.a` | 122,800 | +8 |
| `storage/mylite/libmylite_embedded.a` | 388,440 | 0 |
| `mylite-open-close-smoke` | 7,848,656 | -45,712 |
| stripped `mylite-open-close-smoke` | 5,641,032 | -33,072 |
| `mylite-compatibility-smoke` | 7,722,912 | -45,800 |
| stripped `mylite-compatibility-smoke` | 5,535,872 | -33,152 |
| `mylite_persistent_statistics_stub.cc.o` | 5,376 | n/a |

The first stub attempt made `set_statistics_for_table()` a no-op and returned
zero from range-cardinality helpers. That was wrong: MariaDB uses
`set_statistics_for_table()` for the ordinary non-EITS fallback that copies
handler row estimates into `TABLE::used_stat_records`. Leaving that field at
zero caused `UPDATE` and `DELETE` plans to skip rows. The final stub preserves
the handler-statistics fallback and reports that no engine-independent
statistics are usable.

Verification passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-persistent-statistics \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh \
  tools/run-embedded-bootstrap-smoke.sh \
  tools/run-libmylite-open-close-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```
