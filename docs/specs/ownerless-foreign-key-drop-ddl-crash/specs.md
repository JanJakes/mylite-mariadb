# Ownerless Foreign-Key DROP DDL Crash

## Problem Statement

Ownerless foreign-key crash coverage kills an
`ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` writer after MariaDB/InnoDB
commits native constraint metadata but before MyLite publishes ownerless
dictionary finish. The adjacent DROP FOREIGN KEY boundary still needs direct
evidence: if a writer dies after native InnoDB removes the constraint but before
ownerless dictionary finish, no-live recovery must preserve the completed
constraint removal and must not resurrect stale ownerless enforcement state.

This slice adds focused crash-boundary evidence for a completed DROP FOREIGN
KEY operation.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8051` through
  `mariadb/sql/sql_yacc.yy:8059` parses `ALTER TABLE ... DROP FOREIGN KEY`
  and records `ALTER_DROP_FOREIGN_KEY` in the ALTER flags.
- `mariadb/storage/innobase/handler/handler0alter.cc:123` through
  `mariadb/storage/innobase/handler/handler0alter.cc:126` classifies
  `ALTER_DROP_FOREIGN_KEY` as an InnoDB foreign-key schema operation.
- `mariadb/storage/innobase/handler/handler0alter.cc:4421` through
  `mariadb/storage/innobase/handler/handler0alter.cc:4440` identifies foreign
  keys being dropped during alter validation.
- `mariadb/storage/innobase/handler/handler0alter.cc:8281` through
  `mariadb/storage/innobase/handler/handler0alter.cc:8335` resolves named
  foreign-key drops against InnoDB's existing table foreign-key set.
- `mariadb/storage/innobase/handler/handler0alter.cc:9371` through
  `mariadb/storage/innobase/handler/handler0alter.cc:9418` deletes dropped
  constraints from `SYS_FOREIGN` and `SYS_FOREIGN_COLS`.
- `mariadb/storage/innobase/handler/handler0alter.cc:10046` through
  `mariadb/storage/innobase/handler/handler0alter.cc:10130` adds or drops
  foreign-key constraints in persistent dictionary tables before cache refresh.
- `mariadb/storage/innobase/handler/handler0alter.cc:11701` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11723` commits the InnoDB
  ALTER transaction before post-commit cache refresh.
- `mariadb/storage/innobase/handler/handler0alter.cc:11743` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11767` refreshes InnoDB
  foreign-key cache state after the native ALTER commit.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database with
  `app.ownerless_fk_drop_crash_parent` and
  `app.ownerless_fk_drop_crash_child`,
- define `ownerless_fk_drop_crash_child_parent` during child table creation so
  the pre-crash state includes a real child-to-parent FK,
- insert deterministic parent and child rows, verify FK metadata is visible,
  and prove an orphan child insert fails before the crash,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_fk_drop_crash_child DROP FOREIGN KEY ownerless_fk_drop_crash_child_parent`
  under the existing `dictionary-before-finish` hook,
- kill the writer after native MariaDB/InnoDB DDL completes but before
  ownerless dictionary finish,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify the recovered `information_schema.referential_constraints` and
  `information_schema.key_column_usage` entries for the named FK are absent,
- verify FK enforcement no longer blocks orphan child inserts or parent deletes,
- verify ownerless and native exclusive reopen before and after forced `.shm`
  rebuild observe the same absent-FK metadata, retained rows, child index
  usability, and post-drop DML behavior.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed DROP FOREIGN KEY,
- live-peer cleanup-busy behavior and no-live rebuild,
- recovered absence of FK metadata and enforcement through ownerless/native
  reopen before and after forced `.shm` rebuild.

Out of scope:

- foreign-key rename, cross-schema, composite, generated-column, cyclic, and
  referential-action crash variants,
- randomized FK/RQG oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless foreign-key compatibility by proving that a writer death at MyLite's
dictionary publication boundary preserves completed native DROP FOREIGN KEY
metadata removal and enforcement removal.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises existing InnoDB dictionary storage,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native DROP FOREIGN KEY ALTER machinery.
MyLite does not reinterpret foreign-key metadata; it proves no-live ownerless
recovery rebuilds volatile coordination around the completed native dictionary
removal.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-drop-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata omits the named child-to-parent FK from
  `information_schema.referential_constraints` and
  `information_schema.key_column_usage`.
- Retained parent/child rows and the child parent-id index survive recovery.
- Post-drop orphan child inserts and parent deletes succeed after recovery and
  after reopen.
- Ownerless and ordinary native reopen observe the same absent-FK state before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic DROP FOREIGN KEY crash coverage, not the full FK DDL
  crash matrix.
- Rename, cross-schema, referential-action, generated, composite, and cyclic
  foreign-key crash variants remain separate candidate slices.
- Full external MariaDB/RQG long-running FK stress remains planned.
