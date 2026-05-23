# MTR routed storage online ALTER smoke

## Problem

Public `libmylite` direct and prepared execution reject online and in-place
`ALTER TABLE` forms before MariaDB execution. The raw storage-routed MTR list
bypasses that public policy layer, so it should still prove the embedded MyLite
handler profile does not accidentally accept unsupported online or in-place
DDL and that rejected attempts do not mutate table metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.h` defines `HA_NO_ONLINE_ALTER` and the
  `check_if_supported_inplace_alter()` return values.
- `mariadb/storage/mylite/ha_mylite.h::ha_mylite::table_flags()` advertises
  `HA_NO_ONLINE_ALTER`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::check_if_supported_inplace_alter()`
  returns `HA_ALTER_INPLACE_NOT_SUPPORTED`.
- `mariadb/sql/sql_alter.cc::Alter_info::supports_algorithm()` rejects
  requested `ALGORITHM=INPLACE`, `ALGORITHM=INSTANT`, and `ALGORITHM=NOCOPY`
  when the handler reports no in-place support.
- `mariadb/sql/sql_alter.cc::Alter_info::supports_lock()` and
  `mariadb/sql/sql_table.cc::online_alter_check_supported()` reject
  `LOCK=NONE` when the handler cannot support online ALTER.
- Some raw storage-profile forms can reach MariaDB's temporary table create
  path and report `ER_CANT_CREATE_TABLE` wrapped around the handler's
  unsupported-table-shape return before the public MyLite SQL policy would have
  run.

## Design

Add `mylite.routed_storage_online_alter` to the storage MTR list. The test runs
with a primary `.mylite` file and enforced MyLite storage. It creates one
ordinary routed table, verifies representative online and in-place ALTER forms
fail, then proves supported copy ALTER still succeeds and the failed column
definitions were not published:

- `ALTER ONLINE TABLE ...`;
- `ALTER TABLE ... ALGORITHM=INPLACE`;
- `ALTER TABLE ... ALGORITHM=INSTANT`;
- `ALTER TABLE ... ALGORITHM=NOCOPY`;
- `ALTER TABLE ... LOCK=NONE`; and
- supported `ALTER TABLE ... ALGORITHM=COPY` as the control path.

The test accepts MariaDB's ALTER-not-supported errors plus the raw temporary
table create failure instead of asserting MyLite's stable public diagnostic
because raw MTR does not execute through the `libmylite` SQL policy preflight.

## Scope

This is test and documentation work only. It does not implement online DDL,
in-place, instant, or no-copy ALTER algorithms, weaker lock modes, online copy
ALTER, or a new public diagnostic.

## Compatibility Impact

The storage MTR runner gains direct evidence that raw embedded MyLite storage
sessions reject representative online and in-place ALTER requests while
preserving copy ALTER support. Public `libmylite` direct and prepared SQL
remain the authoritative compatibility surface for stable online ALTER
diagnostics.

## Storage And Lifecycle Impact

Rejected raw online and in-place ALTER requests must not publish blocked column
metadata or create forbidden durable sidecars. The existing table must remain
queryable and able to complete a supported copy rebuild after the rejected
requests.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_online_alter`
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_online_alter`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Representative raw online and in-place ALTER requests fail.
- Supported copy ALTER still succeeds after the rejected attempts.
- Failed column definitions are not visible through `INFORMATION_SCHEMA.COLUMNS`
  or `SHOW CREATE TABLE`.
- The supported routed table remains queryable.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed online ALTER
  coverage without claiming public `libmylite` diagnostics for raw MTR.

## Risks

Raw MTR diagnostics can change if MariaDB changes which ALTER layer reports
unsupported online or in-place DDL first. Public direct and prepared APIs
continue to own stable MyLite diagnostics; this raw smoke test focuses on
failure, metadata stability, copy-ALTER preservation, and sidecar absence.
