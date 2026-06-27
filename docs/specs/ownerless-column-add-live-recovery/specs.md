# Ownerless Column-Add Live Recovery

## Problem Statement

Ownerless column-add crash coverage already kills a writer after MariaDB
finishes a native `ALTER TABLE ... ADD COLUMN` but before MyLite publishes the
ownerless dictionary boundary. That selector currently proves that a live peer
keeps cleanup busy until the peer exits, then no-live recovery rebuilds
coordination around the completed native ALTER.

This slice promotes the focused plain stored-column ADD case to live-peer
dictionary recovery: a later ownerless opener may publish the ownerless
dictionary boundary while another ownerless peer remains open when
pre-execution metadata proves a real ADD branch and the writer reaches the
post-native-success dictionary fault. Because this path writes the native
file-operation checkpoint marker, recovery must retain that marker until
no-live drain rather than treating the ALTER as metadata-only.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:686` through `mariadb/sql/handler.h:742` define
  parser-side `ALTER_PARSER_ADD_COLUMN` and handler-side `ALTER_ADD_COLUMN`
  flags, including the stored base-column subkind.
- `mariadb/sql/sql_table.cc:7313` through `mariadb/sql/sql_table.cc:7328`
  maps newly added non-generated fields to `ALTER_ADD_STORED_BASE_COLUMN`.
- `mariadb/sql/sql_table.cc:10666` through `mariadb/sql/sql_table.cc:10702`
  documents that `mysql_alter_table()` selects direct/in-place or copy ALTER
  based on handler support.
- `mariadb/sql/sql_table.cc:11592` through `mariadb/sql/sql_table.cc:11729`
  fills `Alter_inplace_info` and asks the handler whether an in-place ALTER is
  supported before falling back to copy.
- `mariadb/storage/innobase/handler/handler0alter.cc:1895` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1943` treats stored base
  column ADD as an instant/rebuild-avoiding candidate only for a constrained
  flag set.
- `mariadb/storage/innobase/handler/handler0alter.cc:2607` through
  `mariadb/storage/innobase/handler/handler0alter.cc:2675` documents the
  constant-default requirement for ADD COLUMN before InnoDB can keep the ALTER
  in the non-copy path.

## Design

- Add a dictionary recovery kind for the focused ordinary stored-column ADD
  case.
- Add a conservative SQL classifier for:
  `ALTER TABLE <table> ADD [COLUMN] <column> <single-clause tail>`.
- The classifier accepts only a single ADD clause and requires pre-execution
  `INFORMATION_SCHEMA.COLUMNS` metadata to prove the target column is absent.
- The classifier rejects shapes that require separate proof before live-peer
  recovery can be claimed:
  - `ADD COLUMN IF NOT EXISTS`,
  - `AUTO_INCREMENT`,
  - generated/virtual/stored generated columns,
  - constraints, keys, indexes, and foreign keys,
  - placement clauses (`FIRST` / `AFTER`),
  - explicit `ALGORITHM` / `LOCK` options,
  - multi-action ALTER statements.
- Add the new recovery kind to the native file-operation dead-owner recovery
  lane. Live recovery publishes the dictionary boundary while leaving the
  native file-operation checkpoint marker durable.
- Promote `dictionary-column-add-crash` from cleanup-busy/no-live recovery to
  held-live-peer recovery. The test verifies the native file-operation marker
  is set after the killed writer, remains set while a peer is still live, clears
  after the peer exits and a no-live ownerless reopen drains the checkpoint,
  the recovered `note` column and default are visible, existing rows read the
  default, a follow-up insert works while the peer is still live, and
  ownerless/native reopen plus forced `.shm` rebuild preserve the state.

## Scope And Non-Goals

In scope:

- The existing deterministic crash-at-`dictionary-before-finish` selector for
  `ALTER TABLE app.ownerless_column_add_crash_base ADD COLUMN note INT NOT NULL
  DEFAULT 7`.
- Live-peer recovery for the narrowly classified plain stored-column ADD case.
- Ownerless/native reopen and forced `.shm` rebuild checks for the recovered
  metadata and rows.

Out of scope:

- Broad MariaDB ALTER TABLE grammar support.
- `AUTO_INCREMENT` column add, generated columns, constraint/index/FK DDL,
  placement variants, explicit `ALGORITHM` / `LOCK` variants, and
  multi-action ALTER statements.
- Copy/rebuild ADD COLUMN variants that need broader native file-lifecycle
  checkpoint proof.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL syntax is newly enabled. The slice strengthens ownerless concurrency
evidence for an existing MariaDB-compatible ALTER TABLE form by proving MyLite
can publish the ownerless dictionary boundary after a killed writer while
another ownerless peer remains live.

## DDL Metadata Routing Impact

MariaDB continues to execute and persist the native table-definition change.
MyLite records the conservative recovery kind before statement execution and
uses post-crash native metadata to publish only the ownerless dictionary
boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains inside the
MyLite database directory. The slice exercises existing ownerless dead-owner
cleanup, dictionary generation recovery, live-peer opening, native exclusive
reopen, and `.shm` rebuild lifecycle.

## Native Storage Impact

The covered ADD COLUMN shape remains MariaDB/InnoDB-native. MyLite does not
reinterpret table metadata or page contents; it only coordinates ownerless
dictionary visibility after MariaDB has completed the native ALTER.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-add-crash`.
- Run the broader registered ownerless SQL shards that compile the selector
  surface.
- Run adjacent ownerless dictionary recovery and DDL stress checks.
- Run ownerless primitive recovery-kind coverage, CI production-build guards,
  format check, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- The crashed writer is killed before MyLite publishes dictionary finish.
- A later ownerless opener recovers while the original live peer remains open.
- The native file-operation checkpoint marker stays set while the peer remains
  live and clears after no-live drain.
- Recovered metadata exposes `note INT NOT NULL DEFAULT 7`.
- Existing rows and later inserts observe the default value.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table definition and rows.

## Risks And Unresolved Questions

- The classifier intentionally covers narrower syntax than MariaDB's full ALTER
  TABLE grammar.
- ADD COLUMN forms that rebuild, add keys/constraints, or alter generated or
  AUTO_INCREMENT metadata still require separate proof.
- Broader DDL/file-lifecycle classes, transaction crash windows, active-reader
  crash breadth, and external randomized oracle stress remain completion work
  for ownerless concurrency.
