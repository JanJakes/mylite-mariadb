# Ownerless Compressed Row-Format File-Op Marker Crash

## Problem Statement

Ownerless compressed row-format crash coverage already kills a writer after
MariaDB/InnoDB completes `ALTER TABLE ... ROW_FORMAT=COMPRESSED
KEY_BLOCK_SIZE=8` but before MyLite publishes ownerless dictionary finish, then
proves no-live recovery preserves the rebuilt compressed table. The remaining
durable-boundary gap is narrower: the killed writer should leave the native
file-operation checkpoint-needed marker set while another ownerless peer is
still live, before no-live recovery can checkpoint and clear it.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc` requires an InnoDB
  rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates compressed
  `KEY_BLOCK_SIZE` values and maps compressed row-format options to native
  compressed table state.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` is the native
  InnoDB file-operation redo source that MyLite observes for ownerless
  file-lifecycle checkpoint evidence.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()` calls
  `mark_ownerless_native_file_op_checkpoint_before_dictionary_finish()` before
  the `dictionary-before-finish` hook can stop the writer.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  compressed row-format crash recovery coverage and checkpoint marker readers
  for `concurrency/mylite-concurrency.ckpt`.

## Design

Split the existing compressed row-format crash test into a shared runner with a
boolean marker assertion. The existing selector keeps the recovery-only path.
The new hook-only selector,
`dictionary-compressed-row-format-file-op-marker-crash`, uses the same
`ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` crash shape and asserts
`read_concurrency_native_file_op_checkpoint_needed(database_path)` immediately
after the killed writer reaches `dictionary-before-finish`, while the live peer
still prevents cleanup.

After the marker assertion, the selector preserves the existing recovery
checks: live-peer cleanup remains busy, no-live ownerless reopen observes the
compressed metadata and retained BLOB rows, post-recovery writes succeed, and
ownerless/native reopen before and after forced `.shm` rebuild preserve the
compressed state.

## Scope And Non-Goals

In scope:

- Durable native file-op marker evidence for the focused compressed
  `KEY_BLOCK_SIZE=8` rebuild crash boundary.
- A focused unsafe-hook CTest wrapper.
- Documentation that keeps broader DDL/file-lifecycle recovery partial.

Out of scope:

- Page compression, encryption, external directories, partitioned tables, and
  general tablespace variants.
- Changing MariaDB redo, InnoDB tablespace, MyLite WAL, or checkpoint formats.
- Supporting unsupported compression/encryption/page-compression options.
- External MariaDB/RQG DDL stress.

## Compatibility Impact

No SQL behavior, public API, native format, or directory layout changes. The
slice strengthens evidence for the existing ownerless compressed row-format
support by proving durable checkpoint-drain evidence exists at the crash
boundary.

## Directory And Lifecycle Impact

The test observes the existing native file-op marker in
`concurrency/mylite-concurrency.ckpt` and the existing volatile coordination
state in `concurrency/mylite-concurrency.shm`. No new files are introduced and
all durable state remains inside the MyLite database directory.

## Native Storage Impact

No native storage format changes. MariaDB's compressed rebuild remains
responsible for the table files; MyLite only records durable evidence that a
later no-live native checkpoint drain is required.

## Build, Size, License, And Dependencies

No production binary-size, dependency, license, or public-symbol impact. The
new CTest is registered only when unsafe ownerless test hooks are enabled.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new focused selector:
  `dictionary-compressed-row-format-file-op-marker-crash`.
- Run adjacent compressed and rebuild marker selectors:
  `dictionary-compressed-row-format-crash`,
  `dictionary-row-format-file-op-marker-crash`, and
  `native-file-op-marker-drain`.
- Run the registered focused CTest.
- Build the production `php-embedded-prod` ownerless SQL target and run the
  non-hook compressed row-format crash selector if available through the direct
  command.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the `dictionary-before-finish` hook and kills
  the writer without hanging.
- The native file-op checkpoint marker is set before the live peer is released.
- Live-peer cleanup remains busy until no-live recovery.
- No-live recovery preserves compressed row-format metadata, retained BLOB
  rows, post-recovery writes, ownerless/native reopen, and forced `.shm`
  rebuild behavior.

## Risks And Follow-Up

- This is focused `KEY_BLOCK_SIZE=8` marker evidence. The deterministic
  `KEY_BLOCK_SIZE=1`/`2`/`4`/`16` marker boundary is covered separately in
  `docs/specs/ownerless-compressed-key-block-file-op-marker-crash/specs.md`.
- Broader native redo/checkpoint reconciliation and DDL/file-lifecycle recovery
  remain partial.
