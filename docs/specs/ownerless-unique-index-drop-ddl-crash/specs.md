# Ownerless Unique Index Drop DDL Crash

## Problem

Ownerless hook crash coverage proves completed non-unique secondary-index drop
recovery, unique-index idempotent no-op recovery, and unique-index replacement
recovery. It does not yet kill a writer after MariaDB/InnoDB has completed
dropping an active unique secondary index but before MyLite publishes ownerless
dictionary finish.

Dropping a unique index changes both metadata and user-visible write
semantics: rows that were duplicate-key failures while the index existed must
be accepted after recovery if the completed native drop is the durable state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches top-level `DROP INDEX` through
  `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc` documents that `CREATE|DROP INDEX` are mapped to
  `mysql_alter_table()`, and maps dropped unique secondary indexes to
  `ALTER_DROP_UNIQUE_INDEX` before calling the storage-engine ALTER path.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` includes unique secondary
  index drop in InnoDB's in-place index metadata path, and
  `commit_inplace_alter_table()` commits the native index removal.
- `mariadb/sql/sql_show.cc` exposes unique secondary-index absence through
  `information_schema.statistics`.
- MyLite ownerless dictionary DDL hooks pause at `dictionary-before-finish`
  after native DDL completion and before publishing the ownerless dictionary
  generation.

## Scope And Non-Goals

In scope:

- Add unsafe-hook selector `dictionary-unique-index-drop-crash`.
- Create an InnoDB table with a unique secondary index over `(tenant_id, slug)`.
- Verify duplicate `(tenant_id, slug)` writes fail before the drop.
- Kill a writer after `DROP INDEX ... ON app.<table>` completes natively but
  before ownerless dictionary finish.
- Verify live-peer recovery observes unique-index absence while another
  ownerless peer remains open, retains the native file-operation marker until
  no-live drain, rejects `FORCE INDEX` on the dropped name, and accepts the
  formerly duplicate row shape.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Crash matrices for every unique-index spelling and online option.
- Concurrent duplicate-key races, primary-key rebuilds, generated-column unique
  indexes, prefix unique variants, FULLTEXT/SPATIAL indexes, partitioned
  tables, and external randomized DDL oracles.
- SQL-level table-lock fault injection; prior representative SQL shapes did
  not reach the ownerless table-wait callback.

## Design

Reuse the existing held-live-peer crash helper:

1. Build `app.ownerless_unique_index_drop_crash_base` with rows that are unique
   under `(tenant_id, slug)`.
2. Add `ownerless_unique_drop_crash_idx` as a unique secondary index and prove
   a duplicate `(tenant_id, slug)` insert fails.
3. Run `DROP INDEX ownerless_unique_drop_crash_idx ON ...` under
   `dictionary-before-finish` while a live peer holds the ownerless runtime.
4. Kill the writer at the hook and recover through a new ownerless opener while
   the peer remains live.
5. Verify index absence and forced-index rejection, insert the formerly
   duplicate key shape, and prove the native file-operation marker remains set
   until the peer exits.
6. Release the peer and prove final no-live recovery drains the marker.
7. Recheck that final state through ownerless and native reopen before and
   after forced `.shm` rebuild.

## Compatibility Impact

No SQL feature changes. The slice strengthens partial ownerless DDL crash
coverage for native MariaDB unique-index drop behavior.

## Directory And Lifecycle Impact

No directory layout changes. Durable state remains in the MyLite database
directory. The selector exercises ownerless process cleanup, live-peer
dictionary recovery, final no-live marker drain, shared-memory rebuild, and
native exclusive reopen.

## Native Storage Impact

No native storage format changes. MariaDB/InnoDB owns the index removal and
duplicate-key semantics; MyLite verifies recovery observes the completed native
state.

## Public API, Build, Size, License, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused `dictionary-unique-index-drop-crash`.
- Run adjacent focused hook selectors for secondary-index drop and unique-index
  replacement.
- Run the hook CTest shard that contains the new selector, unless focused and
  adjacent selectors expose a narrower failure.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- The focused selector reaches `dictionary-before-finish` and does not hang.
- Recovery succeeds while another ownerless peer remains live.
- The native file-operation marker remains set while that peer is live and
  drains after final no-live recovery.
- Recovered metadata no longer lists the unique index.
- `FORCE INDEX` on the dropped name fails after recovery.
- The formerly duplicate `(tenant_id, slug)` row shape inserts successfully
  after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same absent-index state and final rows.

## Risks And Follow-Up

- This is deterministic unique-index drop crash coverage, not a replacement for
  external randomized DDL oracles.
- Broader unique-index option crash matrices remain planned.
