# Persistent Statistics Trim

## Problem Statement

MyLite keeps ordinary query planning, `EXPLAIN`, and native engine statistics,
but the default embedded archive still linked MariaDB's persistent
engine-independent optimizer-statistics storage. That subsystem reads and
writes server-owned `mysql.table_stats`, `mysql.column_stats`, and
`mysql.index_stats` metadata, plus JSON histogram storage, which is not needed
for the current embedded database-directory contract.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_statistics.cc` implements persistent statistical table
  reads, updates, DDL cleanup, and histogram-backed selectivity estimates.
- `mariadb/sql/sql_admin.cc` keeps ordinary `ANALYZE TABLE` handler analysis
  separate from engine-independent statistics collection, and collects
  persistent statistics when `ANALYZE ... PERSISTENT FOR ...` requests it.
- `mariadb/sql/sys_vars.cc` defines `use_stat_tables`, `histogram_size`, and
  `histogram_type` as session variables for persistent statistics and
  histogram collection.
- `mariadb/sql/opt_histogram_json.cc` implements JSON height-balanced
  histogram serialization and selectivity helpers used by persistent
  statistics.

## Proposed Design

Add `MYLITE_WITH_PERSISTENT_STATISTICS`, defaulting to `ON` for normal embedded
builds and forced `OFF` in the MyLite baseline. When disabled, `libmysqld`
links `mylite_sql_statistics_disabled.cc` instead of `sql_statistics.cc` and
omits `opt_histogram_json.cc`.

The disabled implementation preserves the symbols retained SQL code expects,
keeps DDL statistic-table cleanup hooks inert, leaves ordinary engine row
estimates in use, and reports no engine-independent statistics or histograms.
`libmylite` starts the embedded runtime with `use_stat_tables=never` and
`histogram_size=0`, rejects direct and prepared
`ANALYZE TABLE ... PERSISTENT FOR ...`, and rejects attempts to enable the
persistent-statistics system variables.

## Compatibility Impact

No supported application data, JSON, GEOMETRY, native storage engine, ordinary
`ANALYZE TABLE`, or `EXPLAIN` behavior is removed. Persistent optimizer
statistics are server-owned tuning metadata in `mysql.*` tables. Applications
that rely on explicitly collecting or tuning MariaDB persistent statistics need
a custom profile or a future compatibility decision.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build measure`: `libmariadbd.a` is
26,402,232 bytes / 25.18 MiB with 698 members, down 72,184 bytes from the prior
26,474,416-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_PERSISTENT_STATISTICS=OFF` appears in the embedded
  CMake cache.
- Confirm `sql_statistics.cc.o` and `opt_histogram_json.cc.o` are absent, and
  `mylite_sql_statistics_disabled.cc.o` is present in `libmariadbd.a`.
- Verify `@@use_stat_tables=NEVER` and `@@histogram_size=0` at startup.
- Verify ordinary `ANALYZE TABLE` and `EXPLAIN` continue to execute.
- Verify direct and prepared persistent-statistics SQL fails through MyLite
  server-surface policy.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The default embedded archive omits MariaDB persistent statistics storage and
  JSON histogram storage.
- Ordinary engine statistics, planning, execution, `ANALYZE TABLE`, and
  `EXPLAIN` remain covered.
- Persistent-statistics collection and enabling variables are rejected
  explicitly by MyLite policy coverage.
- Compatibility, API, roadmap, and build-size documentation describe the
  unsupported surface and measured size impact.
