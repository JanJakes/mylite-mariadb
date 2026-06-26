# Ownerless Foreign-Key DDL Crash

## Problem Statement

Ownerless foreign-key coverage proves already-open peers refresh after
`ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` and `DROP FOREIGN KEY`, and
that recovered databases enforce referential constraints after normal reopen.
The crash matrix does not yet kill an ADD FOREIGN KEY writer after
MariaDB/InnoDB commits the native dictionary change but before MyLite publishes
ownerless dictionary finish.

This slice added focused crash-boundary evidence for a completed ADD FOREIGN
KEY operation. The later
`docs/specs/ownerless-foreign-key-live-recovery/specs.md` slice promotes the
same bounded ADD FOREIGN KEY syntax to live-peer recovery.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6153` through
  `mariadb/sql/sql_yacc.yy:6167` parses table-level `FOREIGN KEY` clauses and
  records the table foreign key in the ALTER key list.
- `mariadb/sql/sql_yacc.yy:7146` through
  `mariadb/sql/sql_yacc.yy:7155` parses `REFERENCES` table targets for foreign
  keys.
- `mariadb/storage/innobase/handler/handler0alter.cc:123` through
  `mariadb/storage/innobase/handler/handler0alter.cc:126` classifies
  `ALTER_ADD_FOREIGN_KEY` as an InnoDB foreign-key schema operation.
- `mariadb/storage/innobase/handler/handler0alter.cc:2295` through
  `mariadb/storage/innobase/handler/handler0alter.cc:2301` documents InnoDB's
  online ADD FOREIGN KEY support condition.
- `mariadb/storage/innobase/handler/handler0alter.cc:3219` through
  `mariadb/storage/innobase/handler/handler0alter.cc:3413` builds the native
  InnoDB foreign-key descriptors, resolves child and parent indexes, and
  validates parent table/index metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc:8468` through
  `mariadb/storage/innobase/handler/handler0alter.cc:8493` allocates and
  gathers added foreign keys during ALTER preparation.
- `mariadb/storage/innobase/handler/handler0alter.cc:11701` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11723` commits the InnoDB
  ALTER transaction before post-commit cache refresh.
- `mariadb/storage/innobase/handler/handler0alter.cc:11743` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11767` refreshes InnoDB
  foreign-key cache state after the native ALTER commit.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create
  `app.ownerless_fk_crash_parent` plus `app.ownerless_fk_crash_child`, with a
  child-side parent-id index but no foreign-key constraint yet,
- insert deterministic parent and child rows and verify no FK metadata is
  visible before ALTER,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_fk_crash_child ADD CONSTRAINT ownerless_fk_crash_child_parent FOREIGN KEY (parent_id) REFERENCES app.ownerless_fk_crash_parent (id)`
  under the existing `dictionary-before-finish` hook,
- kill the writer after native MariaDB/InnoDB DDL completes but before
  ownerless dictionary finish,
- open a new ownerless read/write handle while the live peer remains open and
  finish the recoverable dictionary boundary,
- verify recovered `information_schema.referential_constraints` and
  `information_schema.key_column_usage` metadata,
- verify FK enforcement rejects an orphan child row, then insert a valid child
  row,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same FK metadata, retained rows, child index usability,
  and orphan rejection.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed ADD FOREIGN KEY,
- live-peer recovery for the completed ADD FOREIGN KEY boundary,
- recovered FK metadata and referential enforcement through ownerless/native
  reopen before and after forced `.shm` rebuild.

Out of scope:

- DROP FOREIGN KEY crash recovery, covered separately by
  `docs/specs/ownerless-foreign-key-drop-ddl-crash/specs.md`,
- foreign-key rename, cross-schema, composite, generated-column, cyclic, and
  referential-action crash variants,
- randomized FK/RQG oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless foreign-key compatibility by proving that a writer death at MyLite's
dictionary publication boundary preserves the completed native ADD FOREIGN KEY
metadata and enforcement state.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises existing InnoDB dictionary storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native ADD FOREIGN KEY ALTER machinery.
MyLite does not reinterpret foreign-key metadata; it proves ownerless live
recovery rebuilds volatile coordination around the completed native dictionary
change.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer can remain open while a new ownerless opener recovers the
  completed dictionary boundary.
- Recovered metadata shows the named child-to-parent FK through
  `information_schema.referential_constraints` and
  `information_schema.key_column_usage`.
- Retained parent/child rows and the child parent-id index survive recovery.
- FK enforcement rejects an orphan child row after recovery and after reopen,
  and the rejected row is absent before any later `COMMIT`.
- Post-recovery valid child writes succeed.
- Ownerless and ordinary native reopen observe the same FK state before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic ADD FOREIGN KEY crash coverage, not the full FK DDL
  crash matrix.
- Rename, cross-schema, referential-action, generated, composite, and cyclic
  foreign-key crash variants remain separate candidate slices.
- Full external MariaDB/RQG long-running FK stress remains planned.
