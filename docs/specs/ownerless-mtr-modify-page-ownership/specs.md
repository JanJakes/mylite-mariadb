# Ownerless MTR Modify Page Ownership

## Problem Statement

The ownerless foreign-key graph stress reducer exposed false duplicate-key
errors after the native purge restart fix. A worker could update an InnoDB row
whose unique secondary index key stayed logically unchanged, but the duplicate
probe later saw the old secondary entry as not delete-marked and returned
MariaDB errno 1062.

This was not a SQL retry classification problem. The failing delete-mark path
showed that a secondary-index record could still read as not delete-marked
immediately after `btr_rec_set_deleted<true>()`.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/include/mtr0log.h` writes page bytes before
  calling `mtr_t::set_modified()` through `memcpy_low()`.
- MyLite's ownerless hook in `mtr_t::set_modified()` can refresh a page before
  marking it dirty. If that refresh runs after MariaDB has already copied a
  delete-mark byte, the refresh can replace the frame with the external page
  image and lose the intended in-place mutation.
- `mariadb/storage/innobase/btr/btr0cur.cc` positions B-tree page cursors under
  X/SX leaf latches before callers mutate records such as secondary-index
  delete marks.
- The existing ownerless X/SX latch preparation was refresh-only for explicit
  SQL writers, so a later dirty-page hook could still refresh after the caller
  trusted the positioned record.

## Design

Ownerless X/SX page-latch preparation now acquires real ownerless page-write
ownership instead of refresh-only ownership. That makes any needed external page
refresh happen before B-tree cursor positioning and records the page as owned
by the MTR or transaction, so the later dirty-page hook no longer refreshes
after bytes have been copied.

`btr_rec_set_deleted()` also prepares ownerless page-write ownership before the
low-level delete-mark byte update. In the normal B-tree modify path this is a
duplicate ownership check; in narrower in-place update paths it keeps the
ownerless refresh-before-mutation invariant local to the delete-mark helper.

## Scope And Non-Goals

In scope:

- persistent InnoDB pages latched for ownerless X/SX B-tree modification,
- low-level secondary-index delete-mark mutations,
- preserving MariaDB's native duplicate-key semantics instead of retrying
  unexpected errno 1062.

Out of scope:

- changing SQL duplicate-key retry policy,
- enabling live purge while ownerless hooks are installed,
- claiming all native purge, DDL, or file-lifecycle recovery classes are
  complete.

## Compatibility Impact

No SQL syntax or public C API changes. The compatibility impact is preserving
MariaDB's native secondary-index delete-mark visibility under ownerless
multi-process page refresh, so a later unique-secondary duplicate probe does
not see a stale live key image.

## Test And Verification Plan

- Build the embedded MariaDB archive and focused ownerless SQL test target.
- Run `mylite_ownerless_cross_process_sql_test fk-graph-stress` with
  `MYLITE_OWNERLESS_FK_GRAPH_STRESS_ROUNDS=48`.
- Run the registered FK graph CTest selector.
- Run ownerless primitives, focused ownerless/hook subsets, format check, and
  `git diff --check`.

## Acceptance Criteria

- The 48-round FK graph stress no longer returns unexpected duplicate-key
  errors from `ownerless_fk_graph_root_worker_kind`.
- The stress verifier passes through ownerless reopen, ordinary native reopen,
  and forced `.shm` rebuild.
