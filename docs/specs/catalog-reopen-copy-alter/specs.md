# Catalog Reopen Copy ALTER

## Problem

MyLite can rediscover routed schemas and tables from the `.mylite` catalog
after close/reopen without recreating MariaDB runtime schema directories. Basic
queries and CHECK enforcement work in that state, but some later `ALTER TABLE`
paths still enter MariaDB's in-place ALTER preparation before falling back to
copy rebuilds.

For `ALTER TABLE ... DROP CONSTRAINT` on a catalog-rediscovered MyLite table,
MariaDB prepares an offline in-place candidate and writes a temporary `.frm`
under the schema directory. Because that directory intentionally does not
exist after catalog-only reopen, the statement fails with
`Unknown database 'app'` before the MyLite handler can use the existing
copy-rebuild path.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_alter.cc:Sql_cmd_alter_table::execute()` delegates ALTER
  execution to `mysql_alter_table()` after privilege and parser-state setup.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` opens the source table,
  builds `Alter_table_ctx`, and decides whether to attempt in-place ALTER
  before falling back to copy.
- `mariadb/sql/sql_table.cc:is_inplace_alter_impossible()` is the SQL-layer
  early gate that can force copy ALTER before storage-engine in-place
  preparation is attempted.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` calls
  `create_table_for_inplace_alter()` before it calls
  `handler::check_if_supported_inplace_alter()`.
- `mariadb/sql/sql_table.cc:create_table_for_inplace_alter()` uses
  `TABLE_SHARE::init_from_binary_frm_image()` to build an altered table share.
  In the catalog-only reopen failure, that path reaches `writefile()` and
  raises `ER_BAD_DB_ERROR` because the runtime schema directory is absent.
- `mariadb/sql/handler.cc:handler::check_if_supported_inplace_alter()` treats
  `ALTER_DROP_CHECK_CONSTRAINT` as an offline in-place candidate. That means
  the handler fallback decision comes too late for MyLite catalog-only reopen.
- `mariadb/storage/mylite/ha_mylite.h` advertises `HA_NO_ONLINE_ALTER`, but
  that does not by itself block MariaDB's offline in-place preparation path.

## Design

Force MyLite routed tables onto the copy ALTER algorithm at MariaDB's early
SQL-layer in-place gate. This matches MyLite's current implementation model:
supported ALTER behavior is copy-rebuild based, with catalog metadata and row
state published through the MyLite handler.

Do not recreate runtime schema directories as a workaround. The target behavior
is filesystem-free schema DDL after catalog reopen; any MariaDB runtime files
created only to satisfy old `.frm` assumptions would keep the wrong model alive.

The initial coverage uses the concrete CHECK case that exposed the gap:

- create and alter a routed `ENGINE=InnoDB` table,
- close and reopen without runtime schema directories,
- verify the ALTER-added CHECK still rejects invalid rows,
- drop that CHECK after reopen, and
- verify the formerly invalid row now succeeds.

## Supported Scope

- Copy `ALTER TABLE` on routed MyLite tables after catalog-only reopen when the
  schema directory has not been rehydrated.
- The concrete covered behavior is reopened named table-level CHECK drop.
- Existing same-runtime copy ALTER behavior remains supported.

## Non-Goals

- Implementing in-place ALTER for MyLite storage.
- Rehydrating MariaDB runtime schema directories after reopen.
- Broad coverage for every ALTER clause.
- Failed ALTER rollback when row copy or validation fails.
- Metadata-only ALTER optimizations.

## Compatibility Impact

MyLite keeps MariaDB SQL semantics for supported ALTER clauses while using the
copy algorithm internally. Users should not observe schema-directory
requirements after reopening a `.mylite` file.

If a user explicitly requests a non-copy algorithm for a MyLite table, MariaDB
should continue to reject unsupported combinations rather than silently using a
different algorithm.

## DDL Metadata Routing Impact

The altered table definition remains stored in the MyLite catalog. No durable
`.frm` sidecar or runtime schema directory is required after close/reopen.

## Single-File And Embedded-Lifecycle Impact

No new durable sidecars are introduced. The embedded runtime should still end
with an empty runtime directory after the storage-smoke test closes the
database.

## Public API And File-Format Impact

The public `libmylite` API and MyLite file format do not change.

## Storage-Engine Routing Impact

The early copy decision applies to tables whose effective handler is MyLite.
Requested engines such as `InnoDB`, `MyISAM`, and `Aria` still resolve to the
MyLite handler through the existing routing policy.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package changes are included.

## Binary-Size And Dependency Impact

No dependency is added. The fork delta is a small MariaDB SQL-layer condition.

## Test And Verification Plan

- Extend storage-engine smoke coverage with reopened CHECK DROP after
  close/reopen.
- Verify no runtime schema directory is required for the reopened ALTER path.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, and
  the routed DDL/CHECK/sidecar compatibility harness groups.

## Acceptance Criteria

- Reopened `ALTER TABLE ... DROP CONSTRAINT` succeeds on the covered routed
  MyLite table.
- The dropped CHECK no longer rejects rows after the ALTER.
- The runtime directory is still empty after close.
- Compatibility docs and the roadmap describe filesystem-free copy ALTER after
  catalog reopen as covered for the tested path.

## Risks And Unresolved Questions

- Other ALTER clauses may still contain earlier filesystem assumptions and need
  separate coverage.
- MyLite may later add a native in-place or metadata-only ALTER path. That
  should be designed as a separate storage-engine capability instead of
  reusing MariaDB `.frm` preparation.
- Failed copy ALTER rollback remains broader transaction/DDL rollback work.
