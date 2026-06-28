# Ordinary Ownerless Concurrency Startup Gate

## Problem

Ordinary embedded read/write opens are the hot path for application adapters
such as WordPress' mysqli integration. They do not request
`MYLITE_OPEN_OWNERLESS_RW` or `MYLITE_OPEN_SHARED_READONLY`, but current
`start_runtime()` still prepares and maps the ownerless coordination directory,
shared-memory file, page-version WAL, checkpoint file, process slot, and
ownerless redo evidence on every disk runtime. Process-isolated PHPUnit pays
that full embedded lifecycle repeatedly, so this adds measurable startup and
shutdown cost even when ownerless concurrency is not in use.

The ordinary path must avoid that ownerless startup tax while still failing
closed and recovering correctly when a database directory already contains
ownerless coordination state from an earlier ownerless open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c:mysql_server_init()` and
  `mysql_server_end()` own the embedded server lifecycle that process-isolated
  PHPUnit children repeat.
- `packages/libmylite/src/database.cc:open_impl()` sets
  `db->ownerless_rw_open` only for `MYLITE_OPEN_OWNERLESS_RW` or
  `MYLITE_OPEN_SHARED_READONLY`, then calls `start_runtime()`.
- `packages/libmylite/src/database.cc:start_runtime()` currently calls
  `prepare_concurrency_metadata()`, `prepare_concurrency_shared_memory()`,
  `map_concurrency_shared_memory_for_runtime()`,
  `open_concurrency_page_log_for_runtime()`, and
  `open_concurrency_checkpoint_for_runtime()` for every non-memory runtime.
- `prepare_concurrency_metadata()` creates `concurrency/` and
  `mylite-concurrency.meta`; `prepare_concurrency_shared_memory()` creates or
  validates `mylite-concurrency.shm`, `mylite-concurrency.wal`, and
  `mylite-concurrency.ckpt`, then may rebuild shared volatile state and replay
  retained page-version WAL.
- Ordinary read/write opens already take the exclusive database-directory lock
  through `mylite.lock`, so their first-run system-table bootstrap does not
  need a separate ownerless coordination lock.
- `release_runtime()` currently calls ownerless page-log reclaim and redo
  startup-prefix capture on ordinary non-memory shutdowns even when no
  ownerless files were mapped.
- The existing `ordinary-ownerless-hot-paths` evidence identified these
  fixed ordinary open/close ownerless steps as the remaining open-specific
  performance gap after SQL hot-path parity checks.

## Scope

In scope:

- Gate ownerless coordination setup behind explicit ownerless/shared-readonly
  flags, or behind a fail-closed check that durable ownerless runtime files
  already exist in `concurrency/`.
- Keep ordinary opens of fresh databases from creating `concurrency/` or
  ownerless runtime files.
- Keep ordinary opens of databases with existing ownerless metadata, WAL,
  checkpoint, SHM, or redo-header backup state on the existing recovery path.
- Avoid ownerless redo-prefix capture and ownerless shutdown cleanup on
  ordinary runtimes that did not map ownerless coordination state.
- Update embedded lifecycle tests and docs to make the ordinary and ownerless
  directory layouts explicit.

Out of scope:

- Migrating away previously created but clean ownerless coordination files.
- Changing ownerless WAL, checkpoint, SHM, page-version, dictionary, or lock
  protocols.
- Changing SQL semantics, public C API flags, or the WordPress mysqli adapter.
- Claiming full PHPUnit parity from this single slice without follow-up CI
  samples.

## Design

Add a small startup predicate:

- memory databases never prepare ownerless coordination files;
- explicit ownerless read/write and shared-readonly opens always prepare them;
- ordinary disk opens prepare them only when durable ownerless runtime evidence
  already exists:
  `mylite-concurrency.meta`, `mylite-concurrency.shm`,
  `mylite-concurrency.wal`, `mylite-concurrency.ckpt`, or
  `mylite-redo-header.bin`.

Filesystem inspection errors fail closed by selecting the ownerless recovery
path; the existing validation then returns the precise corruption or I/O error.
The ownerless platform probe metadata and lock files alone do not select the
heavy path because they are not page-version or redo recovery evidence.

When the predicate is false, `start_runtime()` resets any process-global
ownerless hooks, skips ownerless metadata/SHM/WAL/checkpoint preparation,
skips ownerless mapping and process-slot allocation, and uses the ordinary
exclusive database lock for system-table initialization. On close, ownerless
reclaim, native hook reset, and SHM unmap remain active only if the runtime
actually mapped ownerless coordination state. Ordinary non-read-only disk
closes still keep the lightweight MariaDB redo-prefix guard from
`embedded-repeated-open-redo-repair`, because the startup gate must not leave a
fresh ordinary database unable to reopen once ownerless metadata is introduced
later.

## Compatibility Impact

No SQL or public API behavior changes. Ordinary read/write opens remain
exclusive at the database-directory level. Ownerless and shared-readonly opens
continue to create and validate the `concurrency/` directory. Ordinary opens
after ownerless activity still validate and recover ownerless state because the
durable ownerless files select the existing recovery path.

## Directory And Lifecycle Impact

Fresh ordinary database directories now contain only the base MyLite layout
needed for ordinary embedded storage: `mylite.meta`, `mylite.lock`,
`datadir/`, `tmp/`, and transient `run/` while open. The ownerless
`concurrency/` directory is created lazily by ownerless/shared-readonly opens
or retained from earlier ownerless activity.

## Native Storage Impact

No native file format changes. The slice only avoids ownerless coordination I/O
when no ownerless recovery state exists. Native InnoDB recovery remains under
MariaDB's embedded startup and shutdown lifecycle, with ordinary close-time
redo-prefix repair kept as a native restartability guard rather than ownerless
coordination setup.

## Public API Impact

No public API changes.

## Binary Size Impact

No new dependencies or production targets. The implementation is a small
first-party branch in existing `database.cc` lifecycle code.

## Test Plan

- Focused build of `mylite_embedded_open_close_test`.
- Run `mylite_embedded_open_close_test baseline` to verify ordinary fresh
  opens do not create ownerless coordination files and still clean up runtime
  directories.
- Run `mylite_embedded_open_close_test ownerless-directory` to verify explicit
  ownerless/shared-readonly opens still create, validate, and rebuild
  coordination state.
- Run `mylite_embedded_open_close_test ownerless-product-hooks` to verify hook
  installation and cleanup still work.
- Run the production embedded performance probe with reduced iterations to
  compare ordinary warm open/close and process-isolated startup cost after the
  gate.
- Run `tools/check-ci-production-builds`, the production CTest guard, and
  `git diff --check`.

## Acceptance Criteria

- Fresh ordinary opens do not create `concurrency/`.
- Explicit ownerless/shared-readonly opens still create and validate
  `concurrency/` files.
- Ordinary opens of a database with existing ownerless runtime files still
  validate/recover that state.
- No ownerless hooks remain installed after ordinary open/close.
- Focused embedded lifecycle and ownerless hook tests pass.
- Performance probe output shows ordinary warm open/close no longer paying the
  ownerless SHM/WAL/checkpoint setup phases.

## Verification Evidence

Implementation verification on 2026-06-17 used the `embedded-prod` production
preset:

- `cmake --build --preset embedded-prod --target
  mylite_embedded_open_close_test -j2`
- `ctest --preset embedded-prod -R '^libmylite\.embedded-open-close$'
  --output-on-failure`
- `ctest --preset embedded-prod -R
  '^libmylite\.embedded-ownerless-directory-lifecycle$' --output-on-failure`
- `ctest --preset embedded-prod -R
  '^libmylite\.embedded-ownerless-product-hooks$' --output-on-failure`
- `cmake --build --preset embedded-prod --target
  mylite_embedded_performance_probe -j2`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=3
  MYLITE_PERF_SELECT_ITERATIONS=200 MYLITE_PERF_INSERT_ITERATIONS=50
  build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe`

The reduced production probe reported ordinary warm open/close at
`474.221 ms`, with `165.433 ms` in `mysql_server_init()` and `301.127 ms` in
`mysql_server_end()`. Ordinary ownerless metadata, shared-memory preparation,
shared-memory map, page-log open, checkpoint open, redo evidence, close-time
reclaim, redo capture, redo restore, and unmap phase averages were all
`0.000 ms`. Ordinary active-runtime reconnect was `0.829 ms`.

## Risks And Follow-Up

- Existing test databases that already contain clean ownerless runtime files
  still take the safe recovery path. A future cleanup/migration slice can prove
  when those files are empty and removable.
- This does not reduce MariaDB's embedded `mysql_server_init()` and
  `mysql_server_end()` lifecycle cost, which remains the dominant
  process-isolated PHPUnit cost after ownerless setup is removed.
