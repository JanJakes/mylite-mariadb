# Nested Checkpoint Fast Reuse

## Problem

Prepared row-DML statements execute inside nested MyLite storage checkpoints so
each statement can roll back independently inside an active transaction. The
reusable nested checkpoint object already avoids repeated allocation, but hot
successful commits still ran the full `free_statement()` cleanup path even
when the nested statement had no statement-owned caches, catalog images, append
buffers, dirty-page journals, or rewrite-shape caches to release.

Local `prepared-update-components` sampling showed
`mylite_storage_commit_statement()` time underneath the prepared update step,
with the hot samples landing after `free_statement()` returned. The direct
storage component benchmark measured nested statement commit around
`0.104 us/op` before this slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::StorageStatementCheckpoint` wraps
  file-backed prepared row-DML execution in statement checkpoints.
- `packages/mylite-storage/src/storage.c::mylite_storage_commit_statement()`
  commits nested checkpoints by merging dirty-page undo state, closing the
  statement, merging deferred cache retargeting, and then calling
  `free_statement()`.
- `packages/mylite-storage/src/storage.c::free_statement()` clears every
  resource-owning cache before putting a reusable nested checkpoint into the
  thread-local slot.
- The hot prepared point-update path commonly owns only compact buffered-page
  undo entries that are already handled by
  `reset_buffered_page_undos_for_reuse()`.

## Design

- Add a guarded reusable nested-checkpoint cleanup fast path.
- Use it only when the statement is a nested, non-file-owning,
  non-filename-owning checkpoint and the reusable nested slot is empty.
- Require all resource-owning caches and buffers to be empty before skipping
  the general cleanup path.
- Continue resetting buffered-page undo entries through the existing reusable
  undo-list reset helper.
- Leave general statement cleanup unchanged for any statement with cache
  entries, catalog images, append buffers, dirty-page undo state, journal page
  lists, rewrite-shape caches, owned files, owned filenames, or read-statement
  storage.

## Compatibility Impact

No SQL-visible behavior change is intended. The fast path only changes cleanup
work after a successful nested checkpoint commit, and only when the skipped
cleanup functions would have had no owned resources to release.

## Single-File And Lifecycle Impact

No durable `.mylite` format or companion-file lifecycle change. Rollback
preimages, dirty-page journal ownership, recovery journals, and transaction
journals stay on the existing paths.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing policy change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to one guard helper and one
branch in nested statement cleanup.

## Test And Verification Plan

- Run `git diff --check`.
- Run `git clang-format --diff -- packages/mylite-storage/src/storage.c`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine CTest coverage.
- Run the full storage-smoke CTest preset.
- Run storage and prepared update component benchmarks with a 10k-row working
  set.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=storage-row-update-components 10000 1000000`: nested statement begin
  `0.026 us/op`, mutation `0.328 us/op`, nested statement commit
  `0.053 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: bind component
  `0.023 us/op`, step component `2.160 us/op`, reset component `0.023 us/op`.

## Acceptance Criteria

- Existing storage and embedded storage-engine tests continue to pass.
- Nested checkpoint cleanup keeps the full general cleanup path for any
  resource-owning statement.
- Hot prepared row-DML nested commit benchmarks improve or remain neutral.

## Risks And Open Questions

- The fast path is intentionally conservative. If future nested statement work
  adds new resource-owning fields, the guard must be updated before those fields
  can safely use this cleanup path.
