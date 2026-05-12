# RPL Filter Size Profile

## Problem

The aggressive MyLite minsize profile disables user-facing replication and
binary-log core paths, but it still compiles MariaDB's replication filter
implementation in `vendor/mariadb/server/sql/rpl_filter.cc`.

Current baseline after `general1400-collation-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 32,318,588 |
| stripped `mylite-open-close-smoke` | 6,258,424 |

The archive member `rpl_filter.cc.o` is 36,904 bytes. The current linked
runtime only needs a very small subset of the `Rpl_filter` API: startup and
cleanup allocate/delete `Rpl_filter`, and retained table-open and binlog helper
guards call `Rpl_filter::db_ok()`.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/rpl_filter.h` defines `Rpl_filter` as the
  replication and binlog database/table filtering helper.
- `vendor/mariadb/server/sql/rpl_filter.cc` implements table, wildcard-table,
  database, and rewrite-rule parsing plus string formatting for replication
  filter system variables.
- `vendor/mariadb/server/libmysqld/lib_sql.cc` allocates `binlog_filter` and
  `global_rpl_filter` when embedded startup needs them.
- `vendor/mariadb/server/sql/mysqld.cc` defines and deletes the global
  `binlog_filter` and `global_rpl_filter` pointers.
- `vendor/mariadb/server/sql/table.cc` calls `binlog_filter->db_ok()` while
  opening table shares.
- `vendor/mariadb/server/sql/sql_class.cc` exposes
  `thd_binlog_filter_ok()` and retained binlog helper checks that call
  `binlog_filter->db_ok()`.
- `nm -C --undefined-only` on the current `libmariadbd.a` shows only
  `Rpl_filter::Rpl_filter()`, `Rpl_filter::~Rpl_filter()`, and
  `Rpl_filter::db_ok(char const*)` are unresolved from retained non-filter
  objects.

The retained `db_ok()` behavior should be permissive in MyLite's minsize
profile. Replication and binlog writing are disabled, so filtering a database
out of logging would be both unused and misleading.

## Scope

This slice may add a MyLite-owned `MYLITE_DISABLE_RPL_FILTER` aggressive
minsize option that:

- requires `MYLITE_DISABLE_BINLOG_CORE`,
- removes `../sql/rpl_filter.cc` from the embedded SQL source list,
- adds a tiny MyLite-owned replacement object that defines the linked
  `Rpl_filter` constructor, destructor, and `db_ok()` methods, and
- records archive and stripped linked deltas.

## Non-Goals

This slice does not:

- remove replication or binlog parser grammar,
- remove binlog and replication system variables,
- remove the `binlog_filter` or `global_rpl_filter` global pointers,
- implement user-visible replication filters, or
- change non-minsize MariaDB-derived builds.

If broader callers require more of the `Rpl_filter` API in a later build, the
slice should fail at link time instead of silently carrying incomplete behavior
outside the aggressive minsize profile.

## Proposed Design

Add `MYLITE_DISABLE_RPL_FILTER` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it from
`tools/build-mariadb-minsize.sh`.

When enabled:

- define `MYLITE_DISABLE_RPL_FILTER`,
- require `MYLITE_DISABLE_BINLOG_CORE`,
- remove `../sql/rpl_filter.cc` from `SQL_EMBEDDED_SOURCES`, and
- append `mylite_rpl_filter_stub.cc`.

The stub should keep the `Rpl_filter` object layout initialized enough for safe
construction and deletion by using the same constructor member initializer shape
as upstream. `db_ok()` should always return `true`.

## Affected Subsystems

- Embedded SQL source list.
- Embedded startup and cleanup allocation of `binlog_filter` and
  `global_rpl_filter`.
- Table-open binlog filter checks that remain linked in the no-binlog profile.

## Single-File and Embedded-Lifecycle Impact

The slice does not add files and should not create or retain any replication,
binlog, relay-log, GTID, or filter sidecars. It aligns the aggressive profile
with MyLite's embedded no-replication product surface.

## Public API and File-Format Impact

No public `libmylite` API change and no MyLite file-format change.

Replication and binlog filters remain unsupported implementation details of the
aggressive minsize profile.

## Binary-Size Impact

The direct archive upper bound is about 36 KiB from `rpl_filter.cc.o`, minus
the tiny replacement stub. The stripped linked runtime may save less because
section GC already discards many unreferenced `Rpl_filter` methods.

## License, Trademark, and Dependency Impact

This is GPL-2.0-only MariaDB-derived build-profile work. It adds no dependency
and changes no public trademark or packaging surface.

## Test and Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-rpl-filter MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-rpl-filter MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-rpl-filter MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh tools/run-compatibility-test-harness.sh
```

Measure:

- `libmysqld/libmariadbd.a` bytes,
- stripped `mylite-open-close-smoke` bytes,
- `size` section totals,
- `rpl_filter` and `Rpl_filter` symbols retained in the linked smoke, and
- sidecar scan results from the compatibility harness.

## Acceptance Criteria

- The aggressive minsize build passes current smokes and compatibility harness.
- The linked runtime still opens tables successfully with permissive binlog
  filter checks.
- No replication or binlog filter sidecars are introduced.
- Size deltas are recorded in
  `docs/research/production-size-analysis.md`.
- The implementation remains guarded by MyLite minsize macros and does not
  affect non-minsize builds.

## Risks and Unresolved Questions

- The direct win is likely small. If the stripped linked runtime does not move,
  this slice may still be useful archive cleanup but should be marked as a
  marginal attempt.
- Binlog filter system variables are still compiled elsewhere. Removing those
  references is a separate, broader binlog-sysvar slice.
