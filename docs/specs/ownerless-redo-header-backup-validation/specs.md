# Ownerless Redo Header Backup Validation

## Problem

Ordinary native read/write opens may arm ownerless uncheckpointed file-operation
recovery when `concurrency/mylite-redo-header.bin` proves a prior ownerless
runtime suppressed native redo/checkpoint publication. That backup is durable
state, so startup must reject stale or malformed files instead of treating any
same-named file as ownerless recovery evidence.

## Design

Keep the existing production validation boundary:

- the backup header must have the MyLite redo-header magic,
- the backup format and header size must match the current format,
- the recorded payload size must equal the saved 12 KiB MariaDB redo startup
  prefix,
- the recorded InnoDB redo file size must be large enough and within the
  bounded tolerance of the current `datadir/ib_logfile0`, and
- the saved startup prefix must pass the MariaDB physical redo-header and
  checkpoint validation.

Add a hook-only SQL test seam at the ordinary-open decision point. The hook
fires only when a valid redo-header backup is the sole evidence source arming
ownerless uncheckpointed file-operation recovery. It does not fire for ownerless
opens, retained page-version WAL, or the native file-operation checkpoint
marker.

## Compatibility Impact

No public C API or SQL behavior changes. The slice narrows evidence for the
existing ordinary-open recovery bridge: corrupt backup magic, format, header
size, payload size, recorded redo size, saved redo prefix, or truncated file
content is ignored, while the same ordinary open still succeeds through the
normal MariaDB startup path when the current native redo log is valid.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the
  `ownerless-test-hooks` preset.
- Run the focused `redo-header-backup-validation` selector.
- Run adjacent native recovery selectors:
  `native-reclaim`, `native-file-op-marker-drain`, and
  `statement-checkpoint-scheduling`.
- Run the affected ownerless hook CTest shard and formatting/static checks.
