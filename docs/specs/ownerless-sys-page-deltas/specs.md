# Ownerless SYS Page Delta Rejection

## Problem Statement

Ownerless page-version WAL can publish InnoDB `FIL_PAGE_TYPE_SYS`
native-support pages. A SYS-delta experiment looked attractive because the
simple ownerless autocommit insert workload repeatedly publishes a SYS page,
but crash-recovery hooks showed that treating that class like ordinary
index/undo deltas is not safe yet.

This slice records the negative performance result and locks the current
policy in tests: `FIL_PAGE_TYPE_SYS` records stay standalone for both core
system/undo spaces and user tablespaces. It does not add a new page-log record
flag, change the WAL format, elide native-support proof pages, change
snapshot-boundary retention, or broaden concurrency claims.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_TYPE` at
  offset `24`, `FIL_PAGE_UNDO_LOG = 2`, `FIL_PAGE_TYPE_SYS = 6`,
  `FIL_PAGE_TYPE_TRX_SYS = 7`, and `FIL_PAGE_INDEX = 17855`.
- `mariadb/storage/innobase/fsp/fsp0fsp.cc` writes `FIL_PAGE_TYPE_SYS` for
  file-segment system pages. The ownerless hot-path samples identify the
  repeated rollback-segment proof page as an undo-space SYS page, not a
  file-per-table index page.
- `packages/libmylite/src/ownerless_page_log.cc` has a durable non-chained
  delta payload format for selected page classes. It currently admits index
  and undo-log pages, but not SYS pages.
- MyLite's native checkpoint proof treats SYS pages as native-support state.
  That makes SYS page deltas a recovery-sensitive optimization: a byte-exact
  page-log delta decode is not enough evidence that live/native reclaim can
  safely drop the record.

## Decision

- Keep all `FIL_PAGE_TYPE_SYS` page-log records standalone.
- Do not add a SYS-delta record flag or automatic SYS-delta eligibility in this
  slice.
- Add primitive coverage that appends repeated SYS pages for both `space_id=1`
  and `space_id=80`, then verifies the second record is not marked as an index
  or undo delta and reads back byte-identically.
- Keep using existing performance-probe SYS record and payload counters to
  measure the remaining SYS-page contribution.

## Evidence

A reduced 500-row production probe with broad SYS eligibility selected `0.752`
SYS deltas per ownerless autocommit insert and reduced total page-log payload
from `1112.386` to `1081.608` bytes/insert, but that broad form failed the
record-lock-grant crash recovery hook by recovering `SUM(value)=30` instead of
`31`.

A narrower user-tablespace-only attempt avoided the CTest working-directory
failure after native-support proof was tightened, but the same deterministic
record-lock-grant hook still failed from the repository root working directory
with recovered `SUM(value)=30`. Since application working directory must not
affect recovery correctness, automatic SYS page deltas remain rejected.

A guarded 50-row production probe without automatic SYS deltas reported one
undo-space SYS publication per ownerless autocommit insert. That leaves the
SYS-page payload visible as a future optimization target, but it is not safe to
take before broader native redo/checkpoint reconciliation is designed.

## Affected Subsystems

- Ownerless primitive tests.
- Compatibility and ownerless-concurrency documentation.

No MariaDB parser, DDL metadata, SQL semantics, public API, wire protocol,
directory layout, page-log format, or dependency changes are included.

## Compatibility Impact

This slice does not change MySQL/MariaDB SQL results, native InnoDB page
formats, or durable MyLite WAL compatibility. Existing logs continue to decode
through the older standalone, sparse, fill-sparse, index-delta, and undo-delta
paths.

Ownerless concurrency remains partial until broader native redo/checkpoint
recovery, DDL/file lifecycle recovery, pressure-policy coverage, and external
MariaDB/RQG stress are complete.

## Directory And Lifecycle Impact

No durable files are added. SYS page records remain full page-log records inside
the MyLite-owned database directory and follow the existing page-log pruning and
snapshot-boundary contract.

## Binary Size, License, And Dependencies

The implementation is test and documentation only. It adds no dependency or
license surface and has no expected binary-size impact.

## Test And Verification Plan

- Add primitive coverage proving core and user-tablespace SYS pages remain
  standalone and byte-exact.
- Rerun the record-lock-grant crash hook in the CTest working directory and
  from the repository root to prove the rejected SYS-delta path no longer
  changes recovery.
- Run the production primitive test, focused ownerless SQL selectors, hook
  subset, ownerless stress preset, production-build guard, format check, and
  diff whitespace check before pushing.

## Acceptance Criteria

- `FIL_PAGE_TYPE_SYS` page records do not use index or undo delta flags.
- Core and user-tablespace SYS page records read back byte-identically as
  standalone records.
- The record-lock-grant crash hook recovers `SUM(value)=31` under both tested
  working-directory forms.
- Documentation records the rejected SYS-delta evidence and leaves broader
  redo/checkpoint reconciliation as planned work.
