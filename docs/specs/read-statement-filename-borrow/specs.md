# Read Statement Filename Borrow

## Problem

After read-statement object reuse, hot short read scopes still copy and free
the primary filename for every `mylite_storage_begin_read_statement()` /
`mylite_storage_end_read_statement()` pair. The same local benchmark still
spends about 3.7 us/op in read-statement begin/end, while held-read-scope exact
index entry lookup remains about 0.19 us/op.

Trusted handler paths and storage benchmarks already use
`mylite_storage_filename_identity_scope` when the filename bytes are stable
for active statements. Read statements can use that same proof to borrow the
filename pointer instead of allocating a copy, without changing lock, recovery,
or checkpoint validation behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/include/mylite/storage.h` documents that filename
  identity-scope callers must keep the pointed-to filename bytes stable for
  active statements.
- `packages/mylite-storage/src/storage.c::mylite_storage_begin_read_statement()`
  currently allocates or reuses a statement object, then always copies
  `filename` into `statement->filename`.
- `packages/mylite-storage/src/storage.c::active_statement_filename_matches()`
  already uses stored filename identity metadata before falling back to string
  comparison.
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  opens a `Mylite_filename_identity_scope` before the storage read scope.
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` opens a storage
  read scope but does not currently wrap the stable primary filename in a
  filename identity scope.

## Proposed Design

- Add a storage helper that assigns a statement filename by borrowing the
  caller pointer only when the active filename identity scope exactly matches
  that pointer.
- Keep owned filename copies for all callers without an active matching
  filename identity scope.
- Use the helper only for read statements. Checkpoint statements continue to
  keep the existing owned-string behavior unless they already inherit a parent
  filename.
- Add a `Mylite_filename_identity_scope` around durable handler index cursor
  construction so cursor-building read statements can use the same trusted
  primary-file pointer proof as exact unique reads.
- Keep journal-path caches, file-handle caches, recovery checks, locks, and
  checkpoint snapshot validation unchanged.

## Affected Subsystems

- First-party storage implementation in `packages/mylite-storage/src/storage.c`.
- Storage unit tests in `packages/mylite-storage/tests/storage_test.c`.
- MariaDB handler glue in `mariadb/storage/mylite/ha_mylite.cc`.
- Storage architecture and roadmap docs.

## Compatibility Impact

No SQL, public C API, metadata, or storage-engine routing behavior changes.
Filename equality remains byte-for-byte for unscoped callers. Scoped callers
already promise filename-byte stability for active statements.

## Single-File And Lifecycle Impact

No durable file, recovery companion, or lock lifecycle change. Borrowed
filenames are process-local pointers used only while a read statement is active
or being closed.

## Public API And File Format Impact

No public API or file-format changes.

## Storage-Engine Routing Impact

No routing behavior change. The handler addition wraps an already-stable
primary filename pointer for durable cursor construction.

## Wire-Protocol And Integration Impact

No wire-protocol or integration-package impact.

## Binary-Size Impact

Small first-party code-size increase. No new dependency.

## Test And Verification Plan

- Add storage test-hook coverage proving a read statement under filename
  identity scope borrows the filename, while an unscoped read statement keeps an
  owned copy.
- Run `cmake --build --preset dev --target mylite_storage_test`.
- Run `ctest --preset dev --output-on-failure`.
- Rebuild `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` and
  relink `mylite_embedded_storage_engine_test` and `mylite_perf_baseline`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run focused performance phases:
  `storage-read-statements`, `storage-pk-entry-lookups`,
  `storage-pk-row-lookups`, and `prepared-pk-selects`.

## Acceptance Criteria

- Scoped read statements can borrow stable filename bytes.
- Unscoped read statements keep owned filename copies.
- File replacement and checkpoint-cache tests still pass.
- Handler cursor construction preserves the existing read-scope lifetime and
  does not hold locks across SQL statements.
- Local read-statement or point-read benchmarks improve or remain neutral.

## Risks And Unresolved Questions

- The remaining dominant cost is likely still syscall-heavy recovery probing,
  advisory locking, and header reads. This slice deliberately does not weaken
  those safety checks.
- Borrowing relies on the existing filename identity scope contract. Misusing
  that scope with unstable filename storage remains caller misuse.
