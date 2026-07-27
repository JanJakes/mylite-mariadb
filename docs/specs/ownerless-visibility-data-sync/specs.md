# Ownerless Visibility Data Sync

## Problem

The split WordPress PHPUnit job now exposes Docker setup, WordPress fetch,
MyLite PHP extension build, dependency installation, database preparation,
mysqli performance probing, and PHPUnit execution as separate CI steps. Current
focused WordPress evidence keeps the ordinary mysqli path close to trunk when
the same pinned WordPress ref and database storage placement are compared.

The remaining lower-level engine cliff is ownerless autocommit write cost. On
the current branch, a reduced embedded performance probe on 2026-06-08 reported
ordinary autocommit inserts around 1.7k-2.0k ops/s and ownerless autocommit
inserts around 85-111 ops/s across `FULL`, `NORMAL`, and `OFF` durability. The
near-flat durability response shows the bottleneck is not just MariaDB redo
flush policy. Source inspection points at the ownerless commit visibility
bridge: every ownerless page-write transaction still waits for dirty pages
through the commit LSN, then syncs the ownerless page-version WAL and persists
the visible checkpoint.

This slice makes a bounded low-risk optimization to the repeated ownerless
visibility sync path. It does not remove the conservative dirty-page flush
bridge; that broader change needs a separate proof that page-version WAL
coverage is complete for live readers, peer writers, and no-live recovery.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` already runs the WordPress job through separate
  `docker-image`, `fetch`, `build-php`, `dependencies`, `prepare-db`,
  `perf-probe`, and `phpunit` phases.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` defaults WordPress
  databases to ordinary `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE` opens.
  The focused WordPress ownerless gate sets the pre-connect mysqli ownerless
  option through `MYLITE_WORDPRESS_OWNERLESS_RW=1`; ordinary WordPress jobs
  retain the default mode.
- `mariadb/storage/innobase/trx/trx0trx.cc` calls
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` before releasing
  ownerless locks whenever the transaction has a native id, ownerless lock id,
  ownerless page-write id, or tracked modified pages.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` by waiting
  for `buf_flush_wait_flushed(visible_lsn + 1)` and then invoking the
  page-visible callback.
- `packages/libmylite/src/database.cc` implements the page-visible callback by
  syncing `mylite-concurrency.wal`, publishing the visible LSN in the shared
  redo state, and writing `mylite-concurrency.ckpt` durably.
- `packages/libmylite/src/ownerless_page_log.cc` currently uses `fsync()` for
  page-log visibility sync. `packages/libmylite/src/database.cc` currently uses
  `fsync()` when the checkpoint update is marked durable.
- The visibility page log and checkpoint files already exist when commit
  visibility is published. The repeated commit path needs data durability and
  the metadata required to retrieve appended page-log bytes or updated
  checkpoint bytes; it does not need a full metadata sync for unrelated file
  attributes on every commit.
- A traced reduced probe on 2026-06-08 with three insert iterations was
  distorted for absolute timing but showed the shape of the I/O path:
  `pwrite64=833`, `fdatasync=278`, `fsync=31`, with references to
  `mylite-concurrency.wal`, `mylite-concurrency.ckpt`, `ib_logfile0`, and
  `.ibd` files.

## Design

Add local data-sync helpers and use them only for repeated ownerless
visibility-anchor persistence:

- `mylite_ownerless_page_log_sync_at()` uses a data sync when validating and
  syncing the already-open page-version WAL for commit visibility.
- `update_concurrency_checkpoint_lsn(..., durable=true)` uses a data sync for
  the fixed checkpoint LSN payload.
- File creation, truncation, redo-header backup/restore, native file-operation
  marker updates, and other existing full-sync sites remain unchanged.
- Platforms without `fdatasync()` support fall back to `fsync()`.

This keeps the ordering contract unchanged: ownerless FULL visibility still
waits for the native flush bridge, syncs the page-version WAL, publishes shared
visible state, and syncs the durable checkpoint before considering the visible
LSN durable.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, or storage-format behavior changes.
`MYLITE_DURABILITY_FULL`, `NORMAL`, and `OFF` still map to the existing
MariaDB redo policy. This slice only reduces metadata-sync work in the
ownerless visibility anchor path.

## Directory And Lifecycle Impact

No directory-layout change. The same `concurrency/mylite-concurrency.wal` and
`concurrency/mylite-concurrency.ckpt` files remain the durable ownerless
coordination anchors.

## Native Storage Impact

No native InnoDB format change. Dirty-page flushing and page-version
publication ordering stay conservative.

## Build And Performance Impact

The expected impact is modest but safe: repeated ownerless visibility syncs can
use `fdatasync()` on POSIX systems instead of full `fsync()`. This should lower
metadata sync overhead where the filesystem distinguishes the two calls. It
does not solve the larger ownerless autocommit cliff caused by per-commit dirty
page flushing plus page-log/checkpoint publication.

## Test Plan

- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run the reduced embedded performance probe at default full durability and
  record ownerless autocommit throughput.
- Run focused ownerless committed-read selectors to prove live peer visibility
  still observes an autocommit writer.
- Run hook-only page-visible fault selectors to prove crash-boundary behavior
  around visible checkpoint publication remains covered.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08 used the active `ownerless-concurrency`
worktree on host `/tmp` database storage.

- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  local-write-first-read` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  visible-publish-crash` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  visible-checkpoint-crash` passed.
- A reduced full-durability embedded performance probe passed with ordinary
  warm open/close `406.573ms`, ownerless warm open/close `739.930ms`, ordinary
  direct `SELECT 1` `4206.71 ops/s`, ownerless direct `SELECT 1`
  `3610.78 ops/s`, ordinary transactional inserts `1774.66 ops/s`, ownerless
  transactional inserts `1032.27 ops/s`, ordinary autocommit inserts
  `2255.38 ops/s`, and ownerless autocommit inserts `84.78 ops/s`.
- A tiny decoded `strace` run remained too distorted for throughput, but
  confirmed the sync-path shape changed: before this slice the comparable trace
  saw `fsync=31`; after this slice it saw `fsync=15` and `fdatasync=296`, with
  the ownerless WAL/checkpoint references still present.

## Acceptance Criteria

- The ownerless page-log visibility sync and durable checkpoint LSN update use
  data sync with a full-sync fallback.
- Existing ownerless committed-read and page-visible fault coverage still
  passes.
- The embedded performance probe still passes and reports the ownerless
  autocommit rate explicitly.
- Docs record that this is a bounded sync-path optimization, not a removal of
  the conservative dirty-page flush bridge.

## Risks And Follow-Up

- This may be a small win on filesystems where `fdatasync()` and `fsync()` have
  similar cost.
- The main remaining ownerless autocommit optimization is to replace or batch
  the conservative dirty-page flush bridge. That requires a separate proof that
  page-version WAL publication covers all pages needed by live peer reads,
  peer writes, active snapshot readers, forced `.shm` rebuild, and no-live
  recovery before native dirty pages are allowed to remain unflushed at commit
  visibility.
