# MTR routed storage temporary table smoke

## Problem

First-party storage smoke tests cover representative temporary-table catalog
isolation, same-name shadowing, close/reopen cleanup, and temporary OR REPLACE
paths. The raw storage-routed MTR list does not yet exercise that behavior
through MariaDB's embedded test runner, so it lacks an MTR-scale guard that
temporary table DDL stays session-local and does not publish durable user
catalog metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines `HA_LEX_CREATE_TMP_TABLE`; `HA_CREATE_INFO`
  exposes `tmp_table()` for temporary create paths.
- `mariadb/sql/temporary_tables.cc::THD::create_temporary_table()` creates
  session-local temporary table shares and stores them in the THD temporary
  table list.
- `mariadb/sql/temporary_tables.cc::THD::open_temporary_table()` opens those
  shares and marks them as transactional or non-transactional temporary tables
  based on handler capabilities.
- `mariadb/sql/temporary_tables.cc::THD::open_temporary_table(TABLE_LIST *)`
  resolves temporary tables before base tables, which gives same-name temporary
  tables their shadowing behavior.
- `mariadb/sql/sql_table.cc::mysql_create_like_table()` preserves the
  temporary-table create flag when cloning a source shape.
- `mariadb/sql/sql_insert.cc::select_create::prepare()` temporarily removes
  CTAS temporary targets from the THD temporary list while reading sources.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` treats
  temporary handler creates as process-local volatile table state rather than
  durable user-schema catalog publication.

## Design

Add `mylite.routed_storage_temporary_tables` to the storage MTR list. The test
runs with a primary `.mylite` file and enforced MyLite storage. It creates a
durable source table, then proves representative raw temporary-table behavior:

- `CREATE TEMPORARY TABLE ... LIKE` over a routed source table;
- `CREATE TEMPORARY TABLE ... AS SELECT` over a routed source table;
- same-session temporary row visibility;
- same-name temporary table shadowing of a durable routed table;
- durable table visibility restored after `DROP TEMPORARY TABLE`; and
- temporary names visible in raw session metadata while live, then absent from
  `INFORMATION_SCHEMA.TABLES` after `DROP TEMPORARY TABLE`.

The sidecar assertion runs before and after the temporary lifecycle to catch
forbidden durable MariaDB table files in the raw storage profile.

## Scope

This is test and documentation work only. It does not add new temporary-table
grammar support, temporary OR REPLACE coverage, foreign-key temporary table
support, close/reopen MTR coverage, or new public diagnostics.

## Compatibility Impact

The storage MTR runner gains direct evidence that raw embedded MyLite storage
sessions keep representative temporary LIKE, CTAS, and same-name shadowing
behavior session-local while preserving durable table metadata. Live temporary
tables can appear in MariaDB's session-visible information-schema metadata, so
the durable invariant is that they disappear after `DROP TEMPORARY TABLE` and
do not leave durable user-schema records. The broader temporary-table
compatibility claim remains partial and first-party storage tests continue to
own close/reopen cleanup coverage.

## Storage And Lifecycle Impact

Temporary tables must not leave durable user-schema catalog records or create
forbidden durable sidecars. Durable source and shadowed table rows and indexes
must remain visible after temporary drops.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_temporary_tables`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_temporary_tables`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Raw temporary LIKE and CTAS statements succeed under enforced MyLite storage.
- Temporary rows are readable during the creating session.
- A same-name temporary table shadows a durable table until the temporary table
  is dropped.
- SQL-visible temporary names are visible through the raw session metadata while
  live and absent from `INFORMATION_SCHEMA.TABLES` after `DROP TEMPORARY TABLE`.
- The durable table remains queryable with its supported index after temporary
  table drops.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed temporary-table
  coverage without claiming exhaustive temporary-table compatibility.

## Risks

MariaDB temporary-table internals are session-local implementation details. The
raw MTR test intentionally checks SQL-visible behavior, information-schema
post-drop cleanup, durable-table preservation, and sidecar absence instead of
depending on temporary storage identity names.
