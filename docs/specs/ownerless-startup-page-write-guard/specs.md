# Ownerless Startup Page-Write Guard

## Problem

The production embedded performance probe opens multiple fresh ownerless
InnoDB directories in one process so CI can report first-open, same-device
probe, and warm-open timings from production binaries. On the ownerless
concurrency branch, the first fresh ownerless open/close completed, but the
next fresh ownerless InnoDB bootstrap could abort inside InnoDB doublewrite
creation with a misleading system-tablespace-size error. Local diagnostics
showed the doublewrite creation failure was `DB_CORRUPTION` while the
tablespace was autoextending, and ownerless InnoDB hooks were already active.

This is a startup/recovery correctness and timing-data problem. The probe was
not slow at that point; it failed before producing comparable ownerless timing
samples.

## Source Findings

Base: MariaDB `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc` installs ownerless lifecycle hooks
  before `mysql_server_init()` for ownerless opens. When ownerless InnoDB lock
  hooks are needed for recovery metadata, `start_runtime()` can also call
  `install_ownerless_innodb_lock_hooks(g_runtime)` before native startup
  finishes.
- `mariadb/storage/innobase/srv/srv0start.cc` keeps
  `srv_startup_is_before_trx_rollback_phase` true during early InnoDB startup
  and clears it after startup/recovery advances past the protected phase.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` already treats startup/recovery as
  a no-ownerless-work window for redo entry and page-write prepare decisions:
  `mtr_t::ownerless_redo_enter()` and
  `mtr_t::ownerless_page_write_should_prepare()` return while
  `ownerless_page_write_in_startup_or_recovery()` is true.
- `mtr_t::set_modified()` can call `mtr_t::ownerless_page_write_enter()`
  directly after a page becomes dirty. That direct entry path was not protected
  by the same startup/recovery guard, so pre-start ownerless hooks could reach
  page-write lock, refresh, boundary publication, and perf-accounting paths
  during native doublewrite and system-tablespace bootstrap writes.

## Design

Add a central startup/recovery guard at the top of
`mtr_t::ownerless_page_write_enter()`:

- If ownerless hooks are disabled, return as before.
- If `ownerless_page_write_in_startup_or_recovery()` is true, return before
  ownerless page-write counters, lock acquisition, page-version refresh,
  snapshot-boundary publication, or transaction-scoped page-write ownership.
- Leave all post-start ownerless page-write behavior unchanged.

This keeps the existing pre-start lifecycle/recovery hook installation model
intact while making the MTR page-write entry point itself safe for native
startup and redo/log recovery. Splitting pre-start hook bundles more narrowly
may still be useful later, but it is not required for this regression.

## Compatibility Impact

- SQL behavior: unchanged.
- Public C API: unchanged.
- Directory layout and WAL formats: unchanged.
- Native storage behavior: ownerless page-write hooks are inert during InnoDB
  startup/recovery; after startup completes, page-write locking, page-version
  refresh, snapshot-boundary publication, and diagnostics follow the existing
  ownerless policy.
- Performance evidence: the guard prevents the production embedded performance
  probe from aborting during the second fresh ownerless bootstrap, so CI timing
  rows are again measuring production ownerless startup and statement paths.

## Tests And Verification

- Add `test_ownerless_repeated_fresh_innodb_bootstrap()` to the embedded
  ownerless directory suite. It opens a fresh ownerless InnoDB directory,
  creates an InnoDB table, closes and verifies closed ownerless layout, then
  opens a second fresh ownerless InnoDB directory in the same process and
  repeats the create/close checks.
- Run the production embedded open/close CTest so the new regression coverage
  exercises the production embedded archive and first-party release build.
- Run focused ownerless primitive, hook, and stress selectors to check that the
  post-start ownerless page-write paths still operate.
- Run the reduced and default production embedded performance probes to prove
  the CI timing path reaches ownerless first, same-device, and warm samples.
- Run production-build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- Repeated fresh ownerless InnoDB bootstrap in one process succeeds.
- The default production embedded performance probe completes after the first
  ownerless open/close and reports ownerless timing rows.
- Ownerless page-write lock, refresh, boundary publication, and page-write
  diagnostics remain active after InnoDB startup completes.
- No SQL, public API, directory-layout, or WAL-format compatibility claim is
  expanded by this slice.
- Remaining ownerless gaps stay explicit: broader native redo/checkpoint
  reconciliation, DDL/file-lifecycle recovery, active-reader pressure breadth,
  external MariaDB/RQG stress, and write-performance parity are not closed by
  this guard.
