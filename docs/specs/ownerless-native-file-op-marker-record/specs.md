# Ownerless Native File-Op Marker Record

## Problem

`mylite-concurrency.ckpt` persists a native file-operation
checkpoint-needed marker after ownerless DDL or native metadata changes that
require a later no-live InnoDB checkpoint before MyLite can treat the file
lifecycle as fully drained. Before this slice, the marker was a single 8-byte
legacy field. A torn clear could be misread as "no checkpoint needed", while a
torn set could be misread as "checkpoint needed" only by accident.

The marker must fail conservatively. Losing a required native checkpoint can
leave DDL file-operation redo or native metadata lifecycle evidence dependent
on volatile state; doing one extra checkpoint is acceptable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns the marker in `packages/libmylite/src/database.cc` through
  `mark_concurrency_native_file_op_checkpoint_needed()`,
  `read_concurrency_native_file_op_checkpoint_needed()`, and
  `clear_concurrency_native_file_op_checkpoint_needed()`.
- MyLite checkpoint-file creation and growth is handled by
  `prepare_concurrency_checkpoint_file()`, which already extends
  `mylite-concurrency.ckpt` for checksummed checkpoint LSN records.
- MariaDB InnoDB file lifecycle uses native tablespace operations such as
  `fil_delete_tablespace()` in `mariadb/storage/innobase/fil/fil0fil.cc`.
- MariaDB checkpoint handling rewrites FILE_MODIFY/FILE_CHECKPOINT evidence in
  `fil_names_clear()` and can be forced through `log_make_checkpoint()`, exposed
  by `checkpoint_now_set()` in `mariadb/storage/innobase/handler/ha_innodb.cc`.
- MariaDB redo recovery reads checksum-protected checkpoint slots and validates
  scanned redo against checkpoint state in `mariadb/storage/innobase/log/log0recv.cc`.
- MariaDB's CRC32C implementation is already linked into MyLite's embedded
  profile and used by nearby ownerless checkpoint LSN records.

## Scope And Non-Goals

In scope:

- Append two fixed-size native file-op marker records after the existing
  checkpoint LSN records.
- Store a generation, boolean `needed` value, and CRC32C in each marker record.
- Alternate writes between the two records, so a torn latest write falls back to
  a previous valid generation.
- Preserve the legacy 8-byte marker field for old files and older test/tooling
  expectations.
- Read marker records conservatively: highest valid generation wins; if marker
  records are present but no valid record can be proven, treat the marker as
  needed.
- Prove a torn latest clear record does not suppress a pending native checkpoint
  and is repaired by a later ownerless checkpoint drain.

Out of scope:

- Changing the checkpoint recovery header or database UUID binding.
- Changing InnoDB redo, FILE_MODIFY, FILE_CHECKPOINT, or tablespace formats.
- Completing the broader native redo/checkpoint reconciliation design.
- Claiming SQL-level table-lock fault-injection coverage.

## Design

The checkpoint file keeps its existing fields:

- latest LSN at offset 128,
- visible LSN at offset 136,
- legacy native file-operation marker at offset 144,
- two 64-byte checkpoint LSN records beginning at offset 152.

Two 64-byte marker records now follow the LSN record slots. Each record stores:

- magic `MYLCFOP1`,
- format `1`,
- generation,
- needed value as `0` or `1`,
- CRC32C over the bytes before the checksum field,
- zeroed reserved tail bytes.

Marker set and clear both run under the existing checkpoint byte-range lock.
Each write reads the highest valid marker generation, writes the next generation
to the alternating slot, updates the legacy 8-byte field, and fsyncs the
checkpoint file. The fsync is retained because the marker is used as durable
evidence across ownerless process lifetimes.

Reads still acquire the checkpoint lock. A valid highest-generation marker
record is authoritative. If both marker slots are empty, readers fall back to
the legacy 8-byte marker for old files. If any marker slot is non-empty but no
valid marker record exists, the reader reports `needed=true` instead of failing
open. If one marker record is corrupted and another valid record exists, the
valid record is used.

This conservative policy can cause an unnecessary no-live native checkpoint
after a torn clear, but it prevents a torn clear from losing required
file-operation drain evidence.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, or wire-protocol behavior changes. Existing
checkpoint files with empty marker-record slots still read through the legacy
field. New files remain self-contained inside the MyLite database directory.

## Directory And Lifecycle Impact

`mylite-concurrency.ckpt` grows by two 64-byte marker record slots. No new file
is introduced and no durable state leaves the MyLite database directory.

## Native Storage Impact

No native InnoDB page, tablespace, or redo format changes. The slice strengthens
the MyLite-owned evidence that decides whether native InnoDB file-operation
checkpoint drain still needs to run.

## Build, Size, License, And Dependencies

No new dependency, license, or default-profile binary-size impact. The
implementation reuses MariaDB's already-linked `my_crc32c()`.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Add and run
  `test_ownerless_native_file_op_marker_recovers_from_torn_clear_record`.
- Run nearby native file-op marker and checkpoint LSN SQL cases.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- A marker set publishes a valid checksummed marker record and keeps the legacy
  marker set.
- A marker clear publishes a valid checksummed marker record and keeps the
  legacy marker clear.
- Runtime marker reads prefer the highest valid generation.
- A corrupted latest clear record does not suppress a previous valid set record.
- Existing checkpoint files with empty marker-record slots still read through
  the legacy marker.
- A later ownerless checkpoint drain repairs the corrupted record set and clears
  the marker.

## Implementation Evidence

- `mylite-concurrency.ckpt` now reserves two 64-byte native file-op marker
  records after the checkpoint LSN record slots.
- `mark_concurrency_native_file_op_checkpoint_needed()` and
  `clear_concurrency_native_file_op_checkpoint_needed()` now write alternating
  checksummed marker generations and preserve the legacy 8-byte marker field.
- `read_concurrency_native_file_op_checkpoint_needed()` now prefers the highest
  valid marker generation, falls back to the legacy marker only when marker
  records are empty, and reports `needed=true` for corrupt non-empty
  marker-record-only evidence.
- `test_ownerless_native_file_op_marker_recovers_from_torn_clear_record()`
  writes a marker set, writes a clear, corrupts the latest clear record, verifies
  the marker reads as still needed through the previous valid generation, and
  verifies a later ownerless reopen drains and repairs the marker.
- Production `mylite_ownerless_cross_process_sql_test` build passed.
- Focused direct `sql-case` coverage passed for:
  `test_ownerless_native_file_op_marker_clears_without_page_log`,
  `test_ownerless_native_file_op_marker_recovers_from_torn_clear_record`,
  `test_ownerless_native_file_op_marker_drains_after_real_sql_ddl`,
  `test_ownerless_checkpoint_lsn_record_recovers_from_torn_latest_slot`, and
  `test_ownerless_native_checkpoint_reclaims_page_log`.
- Direct `native-file-op-marker-drain` selector and
  `visible-checkpoint-crash` harness coverage passed.
- `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check` passed.

## Risks And Follow-Up

- Extra checkpoint work is possible after torn marker metadata, by design.
- Broader native redo/checkpoint reconciliation remains partial until DDL
  file-operation recovery, native checkpoint proof, page-version compaction,
  and external stress all cover the remaining lifecycle shapes.
