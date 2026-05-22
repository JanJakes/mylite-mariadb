# Reusable Read Statement Filename

## Problem

Hot storage point-read loops repeatedly create short-lived read statements.
The storage layer already keeps one cleaned read-statement object per thread,
but `free_statement()` releases the owned filename before caching that object.
The next read statement for the same file then allocates and copies the same
filename again before it can reuse the cached read file, journal-path cache,
and checkpoint snapshot cache.

This is small compared with MariaDB execution overhead, but it is avoidable
read-scope setup work on the raw storage path.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c::mylite_storage_begin_read_statement()`
  allocates a read statement, assigns a filename, and then runs
  `initialize_read_statement()`.
- `allocate_read_statement()` reuses one thread-local
  `reusable_read_statement`.
- `free_statement()` currently frees `statement->filename` before placing a
  read statement into `reusable_read_statement`.
- Handler paths can use `mylite_storage_begin_filename_identity_scope()` to
  borrow stable filename pointers, but raw storage API callers and the
  `storage-read-statements` performance phase still pay the repeated filename
  allocation.
- Read-statement startup still runs pending-journal checks, read-file identity
  validation, shared locking, and checkpoint snapshot validation. This slice
  does not weaken those checks.

## Design

- Preserve an owned filename when a read statement is cleaned into the
  thread-local reusable slot.
- Let `assign_read_statement_filename()` reuse that owned string when the next
  read statement uses the same filename.
- Keep filename identity scopes preferred: when an active identity scope points
  at the incoming filename, discard the retained owned copy and borrow the
  identity pointer.
- If the next read statement targets a different filename, free the retained
  name and copy the new filename.
- Add storage unit coverage proving a reusable read statement safely replaces a
  retained filename when reused for another `.mylite` file.

## Compatibility Impact

No SQL, storage-engine routing, or public C API behavior changes. Error
behavior remains the same: allocation failure while copying a different
filename still returns `MYLITE_STORAGE_NOMEM`.

## Single-File And Lifecycle Impact

No file-format or companion-file change. The retained filename is transient
thread-local process memory only.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

All durable routed engines that enter raw storage read statements can benefit:
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default engines, and
`ENGINE=MYLITE`. Runtime-volatile `MEMORY` / `HEAP` table reads are not the
target of this slice.

## Binary-Size And Dependency Impact

Small first-party C changes only. No dependency or meaningful binary-size
impact.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run focused storage read-statement unit coverage.
- Run focused routed storage-engine and statement CTest coverage.
- Run the full `storage-smoke-dev` CTest preset.
- Run the storage read-statement performance phase before and after the change.
- Run `git clang-format --diff` for changed C files.
- Run `git diff --check`.

Local verification:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure -R 'mylite-storage|libmylite.embedded-storage-engine|libmylite.embedded-statement'`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test`
- `ctest --preset storage-smoke-dev --output-on-failure -R 'mylite-storage'`
- `tools/mylite-perf-baseline --phase=storage-read-statements 1000 10000`:
  short post-change samples were noisy at `3.710 us/op` and `4.119 us/op`,
  versus `3.908 us/op` in the pre-slice sample.
- `tools/mylite-perf-baseline --phase=storage-read-statements 1000 100000`:
  longer post-change samples were `3.640 us/op` and `3.704 us/op`.
- `tools/mylite-perf-baseline --phase=storage-pk-row-lookups 1000 10000`:
  `4.316 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 100000`:
  `7.508 us/op`.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

## Acceptance Criteria

- Reused read statements do not allocate a new filename when the filename is
  unchanged.
- Reused read statements still replace the retained filename for a different
  file.
- Filename identity scopes still borrow the scoped pointer.
- Cached read-file path replacement coverage still passes.
- Existing storage-smoke tests pass.
- The local storage read-statement benchmark is neutral or improved.

## Risks And Open Questions

This does not remove shared locks, journal checks, path replacement detection,
or header-page reads from read-statement startup. Larger wins still need a
broader file-runtime/read-transaction model or deeper MariaDB prepared-result
path work.
