# Ownerless AUTO_INCREMENT Primary-Key DDL Refresh

## Problem

Ownerless primary-key replacement coverage proves an ordinary clustered-index
change from one key column to another. Ownerless AUTO_INCREMENT coverage proves
cross-process implicit insert reservation, table-option high-watermark refresh,
and adding a new AUTO_INCREMENT primary-key column during a rebuild. A related
allocation edge remains: replacing the primary key of a table that already has
an AUTO_INCREMENT column.

In MariaDB/InnoDB, an AUTO_INCREMENT column must remain indexed. Moving the
clustered primary key away from that column while preserving a unique secondary
index on it is a rebuild-style DDL path that can change table metadata, local
InnoDB AUTO_INCREMENT state, and ownerless high-watermark seeding at the same
time. MyLite needs focused evidence that an already-open ownerless peer sees
the new primary key and still receives the next implicit AUTO_INCREMENT value.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` requires
  the AUTO_INCREMENT column to participate in a key and rejects invalid
  AUTO_INCREMENT table definitions.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` maps a replacement
  primary key to a clustered unique index and rejects unsupported bare primary
  key drops.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` reserves an
  AUTO_INCREMENT interval before the row write path, so a later duplicate-key
  failure can leave a gap that must not be reused.
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `get_auto_increment()` against InnoDB table metadata, while MyLite's
  ownerless AUTO_INCREMENT registry seeds and publishes a table-ID-keyed
  monotonic high watermark across processes.
- Existing ownerless dictionary-generation refresh flushes peer table metadata
  before statements that observe a newer stable generation.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector for replacing the primary key on an
  InnoDB table that already has an AUTO_INCREMENT column.
- Preserve the AUTO_INCREMENT column with a unique secondary index while moving
  the primary key to a different column.
- Verify an already-open ownerless peer sees `PRIMARY` on the replacement
  column, can still force the AUTO_INCREMENT column's secondary index, and gets
  the next implicit AUTO_INCREMENT ID.
- Verify duplicate writes against the replacement primary key fail with MariaDB
  duplicate-key errno and do not let a consumed AUTO_INCREMENT value be reused.
- Verify final state through ownerless/native reopen before and after forced
  `.shm` rebuild, then insert one more implicit row after rebuild.

Out of scope:

- Composite, descending, invisible, ignored, or full algorithm/lock matrix
  variants.
- Concurrent conflicting primary-key rebuild races.
- Partitioned AUTO_INCREMENT tables, which remain rejected by ownerless policy.
- SQL-level table-lock fault injection and external randomized DDL/RQG oracles.

## Design

Add a selector named `primary-key-autoinc-ddl`:

1. Create `app.ownerless_pk_autoinc` with `id INT AUTO_INCREMENT PRIMARY KEY`,
   a unique `code` key, and two rows.
2. Keep an ownerless parent handle open on the original metadata.
3. In a child ownerless process, run one `ALTER TABLE` that drops the original
   primary key, drops the old unique `code` key, adds a unique secondary key on
   `id`, and adds a replacement primary key on `code`.
4. On the already-open parent, verify `PRIMARY` now maps to `code`, the
   secondary `id` key exists, and an implicit insert receives `id = 3`.
5. Verify a duplicate `code` insert fails with errno 1062 after consuming the
   generated `id = 4`.
6. Verify final row and index metadata through ownerless/native reopen before
   and after forced `.shm` rebuild.
7. Insert after forced `.shm` rebuild and verify the next implicit ID is `5`
   and no row reused the consumed `id = 4`.

The slice should require no product-code change if the existing dictionary
refresh and AUTO_INCREMENT registry seeding paths are correct.

## Compatibility Impact

This broadens ownerless AUTO_INCREMENT and primary-key DDL evidence for one
representative clustered-index rebuild involving allocation state. It does not
claim the full primary-key option matrix.

## Directory And Lifecycle Impact

No new files or layout changes. MariaDB/InnoDB owns the native table rebuild
inside the MyLite database directory. The final state is verified through
ownerless and native reopen plus forced volatile shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The slice exercises a clustered
index replacement while retaining a secondary unique index on the
AUTO_INCREMENT column.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `primary-key-autoinc-ddl` plus adjacent `primary-key-ddl`,
  `auto-inc`, `auto-inc-ddl`, and `auto-inc-column-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless SQL CTest, ownerless stress, `format-check`,
  `git diff --check`, and cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the replacement primary key on
  `code` and the retained secondary key on `id`.
- The peer's first implicit insert after the rebuild receives `id = 3`.
- Duplicate replacement primary-key writes fail with errno 1062.
- After forced `.shm` rebuild, a later ownerless implicit insert receives
  `id = 5`, preserving the gap from the failed duplicate-key write.
- Ownerless and native exclusive reopen preserve row counts, sums, max ID, and
  key metadata.

## Risks And Follow-Up

- The slice uses one deterministic key-replacement shape. Broader composite,
  descending, ignored/invisible, and algorithm/lock option variants remain
  planned.
- External randomized DDL/RQG stress remains separate validation work.
