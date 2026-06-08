# Ownerless Single-Owner External Refresh Skip

## Problem

The page-write refresh fast path proves that a still-single-owner ownerless
runtime can skip non-forced external page-write refresh. Production performance
evidence still shows ownerless autocommit inserts paying for page-version,
disk-read, and space-header refresh paths even when no external writer can
exist:

- ownerless autocommit inserts: `194.25 ops/s` for 400 reduced-probe rows;
- page-version read hook calls: `4943`;
- page-write refresh detail calls: `1600`;
- detailed page-version read calls: `1351`;
- detailed space-header refreshes: `792`.

The previous broad skip experiment removed most of that work but exposed a
correctness bug: checkpointed repeatable-read snapshots could start without a
page-version pin, so active-reader pressure did not throttle writes. The
`ownerless-single-owner-page-write-refresh-skip` slice fixed the checkpoint
baseline path and added active-pin and baseline rejection counters. This slice
retries space-metadata refresh skipping under those stronger guards.

Production probes during the slice also tested a broader buffer-pool/page-level
skip. That variant removed counters but increased MariaDB execution time in the
autocommit insert probe, so the final implementation keeps buffer-pool refresh
conservative and skips only space metadata refresh entry points.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  routes space metadata refresh through:
  - `mylite_ownerless_innodb_refresh_external_space_header()`;
  - `mylite_ownerless_innodb_refresh_external_space_allocation()`;
  - `mylite_ownerless_innodb_refresh_external_space_headers()`.
- Buffer-pool page refresh remains conservative because it performs useful local
  maintenance in the single-owner autocommit write probe.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_skip_external_page_refresh_hook()` now requires one active
  process, matching owner generation, zero active page-version pins, and a
  nonzero redo/checkpoint baseline.
- `seed_ownerless_native_checkpoint_baseline()` returns existing checkpoint
  LSNs, so `START TRANSACTION WITH CONSISTENT SNAPSHOT` can publish a
  page-version pin even when the page-version WAL has already been reclaimed.

## Design

Use the existing `ownerless_skip_external_page_refresh()` helper before
space metadata refresh entry points:

- `mylite_ownerless_innodb_refresh_external_space_header()`;
- `mylite_ownerless_innodb_refresh_external_space_allocation()`;
- `mylite_ownerless_innodb_refresh_external_space_headers()`.

The MyLite callback owns the safety proof. A peer open, active snapshot pin,
or missing checkpoint baseline keeps the conservative refresh path.
`refresh_buffer_pool_page()` and forced page-version refreshes stay conservative.

## Compatibility Impact

No SQL, C API, PHP API, or storage-format changes. The optimization is internal
to ownerless InnoDB refresh. Multi-process ownerless writes, active-reader
pressure, and forced refreshes keep the existing conservative behavior.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The proof reads existing
`mylite-concurrency.shm` process, page-version pin, and redo-visibility
segments.

## Native Storage Impact

No native InnoDB format changes. Single-owner refresh skipping avoids redundant
space-header and allocation refresh work before peer participation is possible.

## Build And Performance Impact

The production MariaDB embedded archive must be rebuilt after the native hook
change. Verification should use production build artifacts:

- `tools/mariadb-embedded-build build`;
- `php-embedded-prod`/`Release` for MyLite and timing probes;
- the repository production MariaDB embedded baseline (`MinSizeRel`) for the
  bundled native archive.

Expected performance impact is a reduction in ownerless autocommit insert
page-read and page-write refresh-detail counters without bypassing local
buffer-pool maintenance. The remaining ownerless autocommit cost is expected to
stay in page publication, page-log append/sync, page-visible publication, and
MariaDB execution.

## Test Plan

- Add focused ownerless SQL coverage
  `single-owner-external-refresh-skip` and CTest
  `libmylite.ownerless-single-owner-external-refresh-skip`.
- The focused test enables database and page-write refresh stats, runs a
  single-owner InnoDB insert loop, and proves:
  - skip calls are all allowed;
  - active-pin and baseline blockers stay zero;
  - MyLite page-read calls are zero;
  - detailed page-version, disk-read, and space-header refresh counters are
    zero;
  - rows survive ordinary reopen.
- Rerun active-reader pressure shard 6 to prove checkpointed snapshot pins still
  throttle writes.
- Run adjacent single-owner selectors and representative ownerless SQL
  selectors.
- Run reduced and default production performance probes.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Single-owner insert coverage proves redundant space metadata refresh work is
  skipped.
- Active-reader pressure coverage proves the space metadata skip does not
  bypass live snapshot pins.
- Production performance counters show page-read/detail-refresh reductions for
  ownerless autocommit inserts.
- Docs and compatibility matrix match the implemented scope.

## Risks And Follow-Up

- The slice does not reduce page-image publication count or page-log append
  cost.
- The slice does not address SQL-level table-lock fault injection, broader
  DDL/file-lifecycle recovery, or external randomized MariaDB/RQG stress.
