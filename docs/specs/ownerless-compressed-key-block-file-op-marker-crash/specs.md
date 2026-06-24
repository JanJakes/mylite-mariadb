# Ownerless Compressed Key-Block File-Op Marker Crash

Update: `ownerless-live-compressed-row-format-rebuild-recovery` upgrades the
representative `KEY_BLOCK_SIZE=8` compressed rebuild boundary to live-peer
dictionary recovery. This spec remains the marker/no-live recovery record for
the focused `KEY_BLOCK_SIZE=1`, `2`, `4`, and `16` variants.

## Problem

Ownerless compressed row-format crash coverage already proves no-live recovery
for `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`, `2`, `4`, `8`,
and `16` when a writer is killed after MariaDB/InnoDB completes the table
rebuild but before MyLite publishes ownerless dictionary finish.

The previous file-operation marker slice proved the durable native
file-operation checkpoint-needed marker at that crash boundary for the
representative `KEY_BLOCK_SIZE=8` rebuild. The remaining key-block marker gap
is to prove the same prefinish marker boundary for the `1`, `2`, `4`, and `16`
variants that already have recovery oracles.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `alter_options_need_rebuild()` requires an InnoDB rebuild when ALTER
  specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::create_options_are_invalid()` validates compressed
  key-block values accepted by the current embedded profile.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` emits native
  file-operation redo for the completed tablespace file lifecycle work.
- `packages/libmylite/src/database.cc` marks the ownerless native
  file-operation checkpoint-needed record before the
  `dictionary-before-finish` test hook can interrupt dictionary finish.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has a
  parameterized compressed key-block crash runner with live-peer cleanup-busy
  for the `1`, `2`, `4`, and `16` variants, no-live recovery, compressed
  metadata, prepared BLOB row, ZBLOB page evidence, ownerless/native reopen,
  and forced `.shm` rebuild oracles.

## Design

Extend the existing compressed key-block crash runner with an `assert_marker`
flag. Existing recovery selectors pass `false` and remain unchanged. New
hook-only selectors pass `true` and assert
`read_concurrency_native_file_op_checkpoint_needed(database_path)` immediately
after the killed writer is reaped and before the live peer is released:

- `dictionary-compressed-row-format-key-block-1-file-op-marker-crash`
- `dictionary-compressed-row-format-key-block-2-file-op-marker-crash`
- `dictionary-compressed-row-format-key-block-file-op-marker-crash`
- `dictionary-compressed-row-format-key-block-16-file-op-marker-crash`

Each selector then continues through the existing recovery oracle so the marker
assertion is tied to a recovered compressed table that remains readable and
writable through ownerless and ordinary native reopen.

The later live compressed row-format recovery slice upgrades the separate
representative `KEY_BLOCK_SIZE=8` selector; this slice intentionally leaves
`1`, `2`, `4`, and `16` on the conservative no-live path until those page-size
variants receive their own live-recovery evidence.

## Scope And Non-Goals

In scope:

- Durable native file-operation marker evidence for compressed key-block
  rebuilds at `KEY_BLOCK_SIZE=1`, `2`, `4`, and `16`.
- Focused unsafe-hook CTest registrations with separate timings.
- Documentation that keeps broader DDL/file-lifecycle recovery partial.

Out of scope:

- Product code changes.
- Page compression, encryption, external table directories, partitions,
  tablespace import/discard, or general tablespace variants.
- External MariaDB/RQG stress.

## Compatibility Impact

No SQL feature or public API behavior changes. The slice strengthens the
existing partial ownerless compressed row-format compatibility evidence by
proving every covered compressed key-block crash variant leaves the durable
native file-operation checkpoint marker before no-live recovery drains it.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test observes the existing
marker in `concurrency/mylite-concurrency.ckpt` while another ownerless peer is
still live, then verifies no-live recovery and final reopen paths need no
manual cleanup.

## Native Storage Impact

Native InnoDB compressed table rebuilds remain MariaDB-managed. MyLite only
records durable evidence that completed native file operations require a later
native checkpoint before page-version WAL and marker state can be reclaimed.

## Binary Size And Dependencies

No production dependency or binary-profile impact. The diff adds hook-only test
selectors, CTest entries, and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the four new focused marker selectors directly.
- Run the registered key-block marker CTest filter.
- Run adjacent existing key-block recovery selectors and the
  `KEY_BLOCK_SIZE=8` compressed marker selector.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Each focused selector reaches `dictionary-before-finish` and kills the
  writer without hanging.
- The native file-operation checkpoint-needed marker is durable before the
  live peer is released.
- Live-peer cleanup remains busy for the `1`, `2`, `4`, and `16` variants until
  final no-live recovery.
- No-live recovery preserves compressed metadata, prepared BLOB rows, ZBLOB
  page evidence at the requested key-block size, post-recovery writes,
  ownerless/native reopen, and forced `.shm` rebuild behavior.

## Risks

- This closes marker evidence for the deterministic compressed key-block values
  already covered by recovery tests. It is not a full storage-option matrix,
  and it does not claim live-peer recovery for the `1`, `2`, `4`, or `16`
  page-size variants.
- Broader native redo/checkpoint reconciliation, unsupported storage options,
  and external randomized DDL stress remain planned.
