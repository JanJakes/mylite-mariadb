# Ownerless Page-Log Checksum Handoff

## Problem

Ownerless page publication appends a page-version WAL record while holding the
page-log append lock. The encoder can choose full, sparse, or delta payloads,
but the durable record checksum is always over the reconstructed full page
image. Current ownerless writes compute that checksum inside the append path,
so each appended page pays a full-page scan while the append path is active.

Recent production attribution with ownerless page-publish stats enabled showed
page-log checksum time is a small but repeated cost per autocommit insert.
Moving the checksum calculation out of the page-log append path reduces
append-lock work without changing the WAL record bytes or recovery checks.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::ownerless_page_write_publish()`
  copies the committed page image, runs `buf_flush_update_zip_checksum()` or
  `buf_flush_init_for_writing()`, then publishes that post-write image through
  `mylite_ownerless_innodb_publish_page_version()`.
- `packages/libmylite/src/database.cc::ownerless_innodb_page_publish_hook()`
  performs MyLite ownerless publish checks, optional active-snapshot boundary
  synthesis, page-log append, and page-index publication.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  encodes the payload but stores `record.checksum` as `checksum_bytes()` over
  the full `record_page`. Delta and sparse encodings do not change the checksum
  domain.
- `packages/libmylite/src/ownerless_page_log.cc::record_checksum_matches()`
  validates recovered pages by recomputing the same full-page checksum.

## Design

Add internal page-log append variants that accept a precomputed full-page
checksum:

- direct initialized append with checksum,
- external-snapshot-lineage append with checksum, and
- append-session append with checksum.

The existing append APIs remain unchanged and keep computing the checksum
inside the page-log module. The new variants are used only by the ownerless
InnoDB page publish hook after it has accepted the page for append and after
any snapshot-boundary work has completed. The hook computes the same
`mylite_ownerless_page_log_checksum_page()` value over the already prepared
page image, records that time in database page-publish performance counters,
and passes the checksum to the page-log append call. The append path records a
precomputed-checksum counter and skips the local checksum scan.

The checksum is not trusted across process boundaries and is never stored
without the page bytes. Recovery and readback still validate record bytes
against the durable checksum.

## Scope And Non-Goals

In scope:

- ownerless InnoDB page-version append calls from `database.cc`,
- page-log append/session variants that accept a checksum,
- primitive byte-exact readback coverage for the checksum handoff API,
- production performance counters for handoff count and checksum timing, and
- docs/spec/compatibility updates.

Out of scope:

- changing page-log record format,
- changing page-log recovery or checksum validation rules,
- combining MyLite checksum calculation with InnoDB native page checksums,
- changing snapshot-boundary append behavior, and
- claiming total ownerless throughput parity with ordinary embedded writes.

## Compatibility Impact

No SQL, public C API, PHP/mysqli, wire-protocol, or durable file-format change.
The checksum field in `mylite-concurrency.wal` keeps the same value and
validation semantics.

## Directory And Lifecycle Impact

No files or directories are added. The optimization affects only transient
in-process append behavior for ownerless page-version WAL records.

## Native Storage Impact

Native InnoDB page images and redo behavior are unchanged. The hook computes
the MyLite WAL checksum after InnoDB has prepared the page image for writing.

## Build, Size, License, And Dependencies

No dependency, license, or build-profile change. Binary-size impact is limited
to small internal wrappers and two performance counters.

## Test And Verification Plan

- Add primitive coverage that appends a page with a precomputed checksum,
  reads it back byte-identically, and observes the precomputed-checksum counter.
- Build the production PHP embedded ownerless SQL harness and performance
  probe.
- Run focused primitive and single-owner ownerless commit-path selectors.
- Run a reduced production performance probe with ownerless page-publish stats
  enabled and verify page-log checksum handoff counters are nonzero.
- Run hook-build visible publish/checkpoint crash cases.
- Run ownerless stress focused selectors, production build guards, format, and
  whitespace checks.

## Acceptance Criteria

- The ownerless publish path records precomputed page-log checksum handoff for
  appended page-version records.
- Page-log local checksum timing drops to the fallback-only path while the new
  database counter reports precompute time.
- Existing append APIs continue computing checksums locally.
- Readback and crash-hook recovery still validate committed rows.

## Risks And Follow-Up

- This moves checksum work out of the append path; it does not remove the
  checksum scan entirely.
- Broader page-publication costs remain in native page publication, payload
  encoding, page-log writes, checkpoint publication, and native-support proof
  pages.
