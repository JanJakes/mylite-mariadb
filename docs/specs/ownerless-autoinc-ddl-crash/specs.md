# Ownerless AUTO_INCREMENT DDL Crash

## Problem

Ownerless AUTO_INCREMENT DDL coverage verifies that an already-open peer
observes `ALTER TABLE ... AUTO_INCREMENT = N` high-watermark changes and does
not reuse IDs when a later DDL lowers the option below existing rows. The
remaining crash boundary is a writer killed after InnoDB persists the native
AUTO_INCREMENT metadata but before MyLite publishes ownerless dictionary finish.

This slice adds hook-build recovery evidence for that native metadata boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `commit_set_autoinc()` handles `ALTER TABLE ... AUTO_INCREMENT`, preserves
  monotonic behavior when the supplied value is lower than existing rows, and
  persists the last-used value with `btr_write_autoinc()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` initializes persistent
  AUTO_INCREMENT metadata for newly created InnoDB tables.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL, and
  `ownerless_dictionary_ddl_needs_native_file_op_checkpoint()` marks
  `ALTER TABLE ... AUTO_INCREMENT` for native file-operation checkpoint
  evidence.
- `packages/libmylite/src/database.cc` exposes the unsafe
  `dictionary-before-finish` hook after native SQL execution but before
  ownerless dictionary finish.

## Design

Add an unsafe-hook selector, `dictionary-auto-inc-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `app.ownerless_auto_inc_crash` as an InnoDB table with an
  AUTO_INCREMENT primary key,
- inserts an initial implicit row,
- keeps a live ownerless peer open,
- kills a writer after
  `ALTER TABLE app.ownerless_auto_inc_crash AUTO_INCREMENT = 50` completes
  natively but before ownerless dictionary finish,
- verifies live-peer cleanup remains busy until no-live recovery,
- inserts after recovery and verifies the recovered next implicit id is `50`,
- lowers the option to `2`, inserts again, and verifies MariaDB/InnoDB
  monotonic behavior uses id `51`,
- verifies ownerless/native reopen before and after forced `.shm` rebuild, and
- inserts after forced rebuild and verifies id `52`.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for representative
  `ALTER TABLE ... AUTO_INCREMENT`.
- Recovered native `.frm` and `.ibd` files under `datadir/app/`.
- Post-recovery implicit insert allocation from the recovered high watermark.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Adding an AUTO_INCREMENT column during a rebuild, already covered by
  `docs/specs/ownerless-autoinc-column-ddl-refresh/specs.md`.
- Primary-key replacement on an existing AUTO_INCREMENT column, already
  covered by `docs/specs/ownerless-autoinc-primary-key-ddl-refresh/specs.md`
  and
  `docs/specs/ownerless-autoinc-descending-primary-key-ddl-refresh/specs.md`.
- Exhaustive AUTO_INCREMENT SQL-mode, offset, increment, and multi-source
  replication semantics.
- External MariaDB/RQG long-running allocation stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens the existing ownerless
AUTO_INCREMENT compatibility claim by proving a completed native high-watermark
rewrite survives writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB table files,
ownerless live-peer cleanup blocking, no-live recovery, forced `.shm` rebuild,
and ordinary native exclusive reopen.

## Native Storage Impact

The AUTO_INCREMENT high watermark is native InnoDB metadata. The test relies on
MariaDB's persisted last-used value and MyLite's directory-backed ownerless
AUTO_INCREMENT registry to prove later implicit inserts do not reuse IDs.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-auto-inc-crash`
- Run adjacent AUTO_INCREMENT selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shard, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- The first post-recovery implicit insert uses id `50`.
- Lowering the option after recovery does not reuse IDs; the next implicit
  insert uses id `51`.
- Ownerless and ordinary native reopen observe the same rows before and after
  forced `.shm` rebuild.
- A post-rebuild implicit insert uses id `52`.

## Risks And Follow-Up

- This covers representative successful high-watermark rewrite, not every
  AUTO_INCREMENT option, session offset/increment, or SQL-mode edge case.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned.
