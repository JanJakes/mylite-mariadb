# Ownerless Composite Direction Primary-Key DDL Crash

## Problem

Ownerless composite direction primary-key refresh coverage proves that one
process can rebuild an InnoDB clustered index from `PRIMARY(id)` to
`PRIMARY(tenant_id ASC, code DESC)` and that an already-open peer sees the new
metadata. The hook-build crash matrix already covers a plain primary-key
replacement and an idempotent primary-key no-op, but not a mixed-direction
composite clustered-key rebuild after native metadata is written and before
MyLite publishes ownerless dictionary finish.

MyLite needs a bounded crash-tail proof for this accepted primary-key variant:
if a writer dies after MariaDB completes the composite direction primary-key
rebuild but before ownerless dictionary finish, live peers must keep recovery
sensitive state busy, no-live recovery must rebuild volatile ownerless state,
and the final native clustered-key metadata must remain usable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` in `KEY_PART_INFO::key_part_flag` when an index column is
  declared descending.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, maps replacement primary-key metadata to
  `DICT_CLUSTERED | DICT_UNIQUE`, records per-field descending metadata, and
  treats primary-key direction changes as order-changing DDL.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes each
  key part's `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes `PRIMARY`
  metadata and key-part direction through `information_schema.statistics`.

## Scope And Non-Goals

In scope:

- Add a hook-only selector,
  `dictionary-composite-direction-primary-key-crash`, that kills a writer at
  `dictionary-before-finish` while executing
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)`.
- Verify live-peer cleanup remains busy until the peer exits and no-live
  recovery runs.
- Verify recovered `information_schema.statistics` exposes the two-part
  `PRIMARY`, with `tenant_id` ascending and `code` descending.
- Verify the old `id` column is no longer part of `PRIMARY`.
- Verify forced-index reads, duplicate composite-key rejection, writes that
  duplicate only the old `id`, and final ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Other primary-key option combinations, algorithm/lock variants, and
  alternate crash points.
- AUTO_INCREMENT primary-key replacements, which are covered by separate
  focused refresh slices.
- SQL-level table-lock wait fault injection; prior exploratory SQL shapes did
  not reach the ownerless table-wait callback.
- External randomized DDL/RQG oracle execution.

## Design

- Reuse the existing ownerless dictionary fault hook with
  `MYLITE_OWNERLESS_TEST_FAULT=dictionary-before-finish`.
- Create `app.ownerless_composite_direction_primary_key_base` with
  `PRIMARY(id)` and three rows.
- Crash the writer while it executes the composite direction primary-key
  replacement.
- Use `crash_dictionary_writer_with_live_peer()` so a live peer proves cleanup
  stays busy before no-live recovery.
- After no-live recovery, insert a row that duplicates only the old `id` but
  has a new composite primary-key value, and verify duplicate composite-key
  writes still fail.
- Reuse the existing composite direction primary-key final-state assertion for
  ownerless/native reopen before and after forced shared-memory rebuild.

## Compatibility Impact

This strengthens the partial ownerless DDL crash matrix for an accepted
clustered-index rebuild variant. SQL semantics are unchanged; ordinary
exclusive embedded behavior continues to inherit MariaDB/InnoDB behavior.

The broader ownerless compatibility claim remains partial because this is one
crash point and one primary-key option shape, not the full algorithm/lock,
concurrent-conflict, or external-oracle matrix.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The slice exercises
MariaDB/InnoDB native clustered-index metadata and MyLite's existing ownerless
dictionary-generation recovery path.

## Native Storage Impact

No native storage format changes. The test verifies recovered InnoDB metadata
and duplicate-key enforcement for the final composite direction clustered key.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. Changes are limited to hook-only test code
and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused `dictionary-composite-direction-primary-key-crash` in
  `ownerless-test-hooks`.
- Run adjacent `dictionary-primary-key-crash`,
  `dictionary-primary-key-idempotent-crash`, `primary-key-ddl`,
  `descending-primary-key-ddl`, and `composite-direction-primary-key-ddl`
  selectors.
- Run the hook ownerless CTest subset, ownerless stress, `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer keeps cleanup busy until no-live recovery.
- Recovered ownerless and native reopen expose `PRIMARY(tenant_id ASC,
  code DESC)` and no longer expose `id` as part of `PRIMARY`.
- Duplicate composite-key inserts fail, while inserts that duplicate only the
  old `id` succeed.
- Final state survives forced `.shm` rebuild.

## Risks And Follow-Up

- Broader primary-key algorithm/lock matrices, alternate crash points,
  concurrent-conflict schedules, AUTO_INCREMENT variants, and external
  randomized DDL/RQG stress remain separate work.
