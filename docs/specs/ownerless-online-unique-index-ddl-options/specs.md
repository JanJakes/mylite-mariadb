# Ownerless Online Unique Index DDL Options

## Problem

Ownerless DDL coverage proves explicit online option combinations for ordinary
secondary indexes, and separate unique-index coverage proves create, replace,
idempotent, enforcement, and crash-recovery behavior. The remaining DDL matrix
still lacks a focused case where an already-open ownerless peer observes a
unique secondary index created and dropped through explicit online ALTER
options.

This slice adds that bounded evidence without adding a new slow selector.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_alter.cc` validates requested `ALGORITHM=` and `LOCK=`
  clauses against the handler's in-place capability.
- `mariadb/sql/sql_table.cc` maps added and dropped unique secondary indexes to
  `ALTER_ADD_UNIQUE_INDEX` and `ALTER_DROP_UNIQUE_INDEX` before calling the
  storage-engine online ALTER path.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` includes unique index
  creation in `INNOBASE_ONLINE_CREATE`, rejects unsafe combinations such as
  `IGNORE` with unique index creation, and returns NOCOPY/COPY in-place
  capability according to rebuild and online-lock requirements.
- `mariadb/sql/sql_show.cc` exposes unique index state through
  `information_schema.statistics.NON_UNIQUE`.

## Scope And Non-Goals

In scope:

- Extend the existing `online-ddl-options` selector with
  `ALTER TABLE ... ADD UNIQUE INDEX ..., ALGORITHM=INPLACE, LOCK=SHARED`.
- Drop the same unique index with
  `ALTER TABLE ... DROP INDEX ..., ALGORITHM=INPLACE, LOCK=SHARED`.
- Verify an already-open ownerless peer sees `NON_UNIQUE = 0`, can use the
  index with `FORCE INDEX`, and rejects a duplicate key while the index exists.
- Verify the peer sees the unique index disappear after drop, and final
  ownerless/native reopen plus forced `.shm` rebuild keep it absent.

Out of scope:

- Exhaustive unique-index online option matrices.
- Concurrent duplicate-key races, primary-key rebuilds, FULLTEXT/SPATIAL
  indexes, partitioned tables, crash injection during this option pair, and
  randomized external DDL oracles.
- SQL-level table-lock fault injection; prior SQL-shape exploration did not
  reach the ownerless table-wait callback.

## Design

Reuse `test_ownerless_online_ddl_options_refresh_peer_dictionary()` because it
already keeps a parent ownerless handle open while a child executes synchronized
DDL boundaries. Add two stages near the existing index-option matrix:

1. The child adds
   `ownerless_ddl_options_unique_status_value_idx(status, value)` with
   `ALGORITHM=INPLACE, LOCK=SHARED`.
2. The parent verifies two `information_schema.statistics` rows with
   `NON_UNIQUE = 0`, verifies a forced-index read, and verifies an attempted
   duplicate `(status, value)` insert fails.
3. The child drops that unique index with the same explicit option pair.
4. The parent verifies metadata absence.

Final-state assertions add the unique-index absence check to the existing
ownerless/native reopen and forced shared-memory rebuild coverage.

## Compatibility Impact

No SQL behavior changes. The slice strengthens the evidence that accepted
MariaDB unique secondary-index ALTER option spellings publish through the
ownerless dictionary refresh path. Unsupported ownerless special-index and
partition surfaces remain unchanged.

## Directory And Lifecycle Impact

No directory layout change. The covered DDL uses native InnoDB index metadata
inside the MyLite database directory and the existing ownerless dictionary
generation refresh boundary.

## Native Storage Impact

No native storage format changes. MyLite relies on MariaDB/InnoDB for unique
secondary-index metadata and duplicate-key enforcement, and verifies that
already-open peers refresh to the native state.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `online-ddl-options`.
- Run the CTest shard that owns the focused selector.
- Run relevant ownerless hook/embedded smoke if the edit changes shared helper
  behavior.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- The already-open ownerless peer observes the online unique index with
  `NON_UNIQUE = 0`.
- Duplicate-key enforcement is active while the unique index exists.
- The peer observes index absence after the online drop.
- Final ownerless/native reopen before and after forced `.shm` rebuild observe
  the same absent-index state and retained rows.
- Docs keep external randomized DDL oracles and broader unique option matrices
  as planned follow-up work.

## Risks And Follow-Up

- This is a deterministic option-pair test, not exhaustive unique online DDL
  coverage.
- External MariaDB/RQG DDL stress remains planned.
