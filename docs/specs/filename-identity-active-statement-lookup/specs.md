# Filename Identity Active Statement Lookup

## Problem

Prepared primary-key update profiling still shows filename comparison work
under `open_existing_file_scope()` and `open_existing_file_for_update_scope()`.
Those helpers first try to reuse the active statement-owned `FILE *`, but
top-level storage checkpoints own a copied filename while the handler passes
the stable `mylite_primary_file_path()` buffer on every row operation. Pointer
equality therefore misses and the hot path falls back to `strcmp()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_primary_file_path()` returns the
  stable process-local MyLite primary filename used by the handler.
- `ha_mylite::external_lock()`, `ha_mylite::read_exact_unique_index_row_into()`,
  and `ha_mylite::update_row()` are the hot prepared point-update lock, read,
  and write paths.
- `packages/mylite-storage/src/storage.c::active_statement_for()` compares the
  caller filename against each active statement filename before reusing the
  statement file handle.
- Storage statements already keep an owned filename copy for correctness and
  journal path derivation.
- The existing table-name identity scope proves the pattern for trusted handler
  string buffers while raw storage callers keep owned-string fallback behavior.

## Design

- Add a storage filename identity scope for trusted callers that can prove the
  filename buffer is stable while active statements may cache its identity.
- Store a borrowed filename identity pointer on active statements only while an
  explicit matching filename identity scope is active.
- Let owner-scoped active statement lookup compare the borrowed identity before
  falling back to the existing pointer-or-`strcmp()` filename comparison.
- Inherit the borrowed identity across nested checkpoints that alias the parent
  filename.
- Use the scope around handler write locks, exact unique reads, and row
  updates, where the primary filename pointer is stable.
- Keep owner-agnostic conflict checks on the existing content comparison path.

## Compatibility Impact

No SQL behavior, storage-engine routing, metadata, public `libmylite` API, or
durable file-format change. Filename equality remains byte-for-byte unless a
trusted caller explicitly opts into a stable filename identity scope.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle change. Statement-owned filenames
remain owned copies, journal paths still use the owned filename, and borrowed
identity pointers are never freed or dereferenced for content.

## Binary-Size And Dependency Impact

Small first-party storage and handler change. No new dependency or build-profile
change.

## Tests And Verification

- Built `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuilt `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` and
  relinked the embedded smoke binaries. The build still emits existing upstream
  missing-`override` and libtool no-symbol warnings.
- Added storage coverage proving a scoped statement does not make an inactive
  mutable filename buffer match a different file.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran focused prepared-update benchmarks. The sampled run measured 2.548 us/op;
  three unsampled follow-up runs measured 2.346, 2.340, and 2.367 us/op.
- Sampled the focused prepared-update benchmark. The active read/update file
  opening frames no longer showed filename `strcmp()`; remaining `strcmp()`
  samples were under durable live-row-id cache lookup and MariaDB table setup.
- Passed `git diff --check` and `git clang-format --diff` on the touched C/C++
  files.

## Acceptance Criteria

- Handler write locks, exact unique reads, and row updates can reuse active
  statement file handles through filename identity when the trusted primary
  filename pointer is active.
- Raw storage callers without a filename identity scope keep the existing
  content-comparison behavior.
- Nested checkpoints inherit safe filename identity metadata from their active
  parent.
- Existing storage and embedded storage-engine tests pass.

## Risks And Open Questions

- The optimization relies on callers using filename identity scopes only for
  filename buffers that stay stable while active statements may cache them. The
  handler's primary filename satisfies this; raw callers with mutable buffers
  should not opt in.
- Broader filename interning may eventually be cleaner, but this narrower
  scope avoids changing storage ownership or public `libmylite` handles.
