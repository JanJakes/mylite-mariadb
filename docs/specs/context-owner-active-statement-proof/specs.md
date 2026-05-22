# Context Owner Active Statement Proof

## Problem

Prepared primary-key update profiling still shows
`mylite_storage_statement_active()` under
`ha_mylite::read_exact_unique_index_row_into()` and
`ha_mylite::external_lock()`. The exact-read scope elision and write-lock
checkpoint proof already skip work when the handler transaction context owns a
statement or transaction checkpoint, but libmylite row-DML checkpoints are owned
by the storage context owner rather than the handler THD transaction context.

The handler therefore falls back to a filename-based storage-chain walk even
when libmylite has already made the current database handle the active storage
context owner and that owner has an active statement checkpoint.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` wraps direct and prepared execution in
  `StorageContextScope`, setting the storage context owner to the current
  `mylite_db`.
- libmylite prepared row-DML can open a storage statement checkpoint before
  calling into MariaDB embedded execution.
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  calls `mylite_thd_has_active_storage_checkpoint()` before deciding whether a
  read-statement scope is needed.
- `mariadb/storage/mylite/ha_mylite.cc::external_lock()` and
  `mylite_begin_statement_checkpoint()` also need to decide whether a storage
  checkpoint is already active.
- `mylite_thd_has_active_storage_checkpoint()` checks handler-owned statement
  and transaction checkpoints first, then falls back to
  `mylite_storage_statement_active(primary_file)`, which compares filenames.
- `packages/mylite-storage/src/storage.c` already tracks the active context
  owner and active statement stack in thread-local storage.

## Design

- Add `mylite_storage_context_has_active_statement()` to report whether the
  current non-null storage context owner owns the active statement stack head.
- Use that context-owner proof in the handler before falling back to the
  filename-based `mylite_storage_statement_active(primary_file)` lookup for
  exact unique reads, write locks, and statement checkpoint setup.
- Keep the existing filename fallback for raw embedded execution, null storage
  owners, and any context where handler or storage owner state does not prove an
  active checkpoint.

## Affected Subsystems

- libmylite prepared row-DML execution.
- MyLite handler exact unique index reads, write locks, and statement
  checkpoint setup.
- Storage context-owner tracking.

## Compatibility Impact

No SQL, storage-engine routing, metadata, or file-format behavior changes. The
handler only skips a redundant checkpoint or read-statement scope when an
active storage context owner already proves that libmylite has a checkpoint
open for the current database handle.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle change. Existing statement,
transaction, savepoint, journal, and rollback behavior remains unchanged.

## Public API And File-Format Impact

The first-party storage package gains one small context-owner query. It does
not change libmylite's public C API or the durable file format.

## Binary-Size And Dependency Impact

Small first-party storage and handler change. No new dependency or build-profile
change.

## Tests And Verification

- Built `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuilt `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` and
  relinked the embedded smoke binaries. The build still emits existing upstream
  missing-`override` and libtool no-symbol warnings.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`; prepared primary-key updates measured
  2.426 us/op on this local run.
- Sampled the focused prepared-update benchmark. The exact-read and write-lock
  checkpoint proof no longer sampled under `mylite_storage_statement_active()`;
  remaining proof samples were under
  `mylite_storage_context_has_active_statement()`.
- Passed `git diff --check` and `git clang-format --diff` on the touched C/C++
  files.

## Acceptance Criteria

- libmylite-owned row-DML checkpoints can prove active storage ownership
  without a filename walk.
- Raw embedded or ambiguous handler contexts keep the existing filename-based
  storage fallback.
- Exact unique reads and write checkpoints outside active checkpoints still
  create their storage scopes through the existing path.
- Existing storage and embedded storage-engine tests pass.

## Risks And Open Questions

- The context-owner proof intentionally requires a non-null owner. Raw embedded
  execution with the default null owner stays on the conservative filename
  lookup path.
