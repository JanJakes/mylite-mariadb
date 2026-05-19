# Storage Full Error Classification

## Problem

Large update benchmarks exposed a misleading failure mode: once the append-only
primary file exhausted available space, physical writes returned a generic
storage I/O error. The MyLite handler already maps `MYLITE_STORAGE_FULL` to
MariaDB's file-full handler error, but no-space and file-size-limit failures in
the low-level write path were collapsed to `MYLITE_STORAGE_IOERR`, which made
MariaDB report the table or index as crashed.

MyLite must distinguish capacity exhaustion from corruption or generic device
I/O failures so application diagnostics and rollback paths stay accurate.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_storage_to_handler_error()`
  maps `MYLITE_STORAGE_FULL` to `HA_ERR_RECORD_FILE_FULL` and
  `MYLITE_STORAGE_IOERR` to `HA_ERR_CRASHED`.
- `packages/libmylite/src/database.cc::map_storage_result()` maps
  `MYLITE_STORAGE_FULL` to the public `MYLITE_FULL` result.
- `packages/mylite-storage/src/storage.c::write_file_at()` writes primary-file
  pages with `pwrite()` and currently maps all non-interrupted write failures
  to `MYLITE_STORAGE_IOERR`.
- `packages/mylite-storage/src/storage.c::write_page()` and `flush_file()`
  cover sequential empty-file and rollback-journal writes, and can see delayed
  no-space errors from stdio flush or `fsync()`.
- `packages/mylite-storage/src/storage.c::truncate_file_to_header_page_count()`
  is part of rollback and recovery cleanup and should preserve capacity-related
  error classification when the platform reports one.

## Design

Add one local errno classifier in the storage layer.

- Map `ENOSPC`, `EFBIG`, and `EDQUOT` where available to
  `MYLITE_STORAGE_FULL`.
- Keep all other physical I/O failures as `MYLITE_STORAGE_IOERR`.
- Apply the classifier to primary-file `pwrite()`, sequential `fwrite()`,
  `fflush()`, `fsync()`, and `ftruncate()` paths.
- Preserve interrupted `pwrite()` retry behavior.
- Preserve the original storage result from created-file cleanup instead of
  replacing a capacity failure with a generic I/O error after removing the
  partial file.

## Compatibility Impact

SQL behavior does not become more permissive. The change improves diagnostic
accuracy: routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted
engine, and `ENGINE=MYLITE` tables continue to use MyLite storage, but
capacity exhaustion now surfaces as a file-full condition instead of a crashed
table or index condition.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Recovery journals keep the same names
and cleanup behavior. If no-space interrupts a mutating operation after a
journal is created, the next storage open still uses the existing recovery
path to restore the previous committed view.

## Public API And File-Format Impact

The public API already exposes `MYLITE_FULL`; this slice makes more physical
capacity failures reach that existing result. No ABI or file-format change.

## Storage-Engine Routing Impact

No routing policy changes. The handler error mapping already supports the
desired routed-engine behavior.

## Binary-Size And Dependency Impact

Small first-party C code only. No new dependency.

## Tests And Verification

- Add a storage unit regression that uses a process file-size limit to make an
  append fail with `EFBIG` without filling the developer machine's disk.
- Verify the failed append returns `MYLITE_STORAGE_FULL`.
- Verify reopening the same file recovers through the existing journal path and
  leaves the table at the previous committed row set.
- Run storage-smoke unit and embedded storage-engine tests.
- Run the local update-only performance baseline at a modest count to verify
  the harness path still works.
- Run `git diff --check`.

## Acceptance Criteria

- Physical no-space and file-size-limit write failures return
  `MYLITE_STORAGE_FULL`.
- Generic non-capacity I/O failures continue to return `MYLITE_STORAGE_IOERR`.
- Recovery after a capacity failure leaves the previous committed view intact.
- Existing storage and routed-engine tests pass.

## Risks And Open Questions

- This does not solve append-only file growth. Free-space reuse, page
  recycling, or a pager/WAL design remains required for SQLite-like sustained
  update performance and bounded database size.
- Some filesystems report capacity failures late, during flush or fsync. The
  classifier covers those paths, but exact reporting remains platform-specific.
