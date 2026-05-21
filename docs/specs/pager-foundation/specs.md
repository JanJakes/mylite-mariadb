# Pager Foundation

## Problem

MyLite has improved append-only scan paths with caches, published index leaf
runs, and active append buffering, but SQLite-like row-write and point-read
performance still needs a real pager boundary. Maintained B-tree indexes and
row directories need page-level reads, dirty page ownership, root-page updates,
rollback, and future free-space reuse. Implementing those directly inside each
row or index feature would duplicate recovery decisions and make write
concurrency harder.

The next storage slice should introduce a narrow pager foundation before
maintained B-tree pages. The first pager slice should not change SQL-visible
behavior; it should centralize page access and transaction ownership so later
index and row-directory work can update pages without republishing catalog
metadata for every row mutation.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`,
  `update_row()`, and `delete_row()` call first-party MyLite storage APIs after
  MariaDB has produced row buffers and key tuples.
- `ha_mylite::index_read_map()`, `index_next()`, `index_next_same()`,
  `index_first()`, and `index_last()` currently consume materialized MyLite
  index cursors; later maintained-index work must preserve that handler
  contract.
- `packages/mylite-storage/src/storage.c::read_page_at()` and
  `write_page_at()` are the current low-level fixed-page I/O boundary. They
  already consult active append buffers for unpublished pages and route header
  reads/writes through active checkpoint state.
- `begin_recovery_journal()`, `begin_write_journal_for_statement()`,
  `publish_header_page_count()`, `flush_append_page_buffer()`, and
  checkpoint commit/rollback helpers own the current append-only durability
  model.
- `packages/mylite-storage/src/storage_format.h` already has page families for
  header, catalog, row, row-state, index-entry, index-leaf, free-list, blob,
  autoincrement, and rollback-journal pages. B-tree or row-directory pages need
  a common access path rather than one-off read/write helpers.
- Existing durable caches are thread-local and view-keyed. They are useful
  read-through accelerators, but they are not a pager because they do not own
  dirty pages, rollback, root publication, or free-space allocation.

## Design

Introduce an internal `mylite_storage_pager` as a first-party storage object
owned by one open file scope or active checkpoint. The initial pager is narrow:

- wrap fixed-size page reads and writes behind pager helpers while preserving
  the existing `read_page_at()` / `write_page_at()` implementation underneath;
- bind each pager to one filename, `FILE *`, header view, and optional active
  statement owner;
- read through the active append buffer and active header state exactly as
  current page reads do;
- expose explicit operations for reading an existing page, appending a new
  page, marking a page dirty, flushing dirty appended pages, and discarding
  transient state on rollback;
- keep page validation in the page-family decoders, not in the generic pager;
- keep journal selection explicit so current recovery journals, transaction
  journals, and active statement checkpoints remain in charge of durability;
  and
- migrate one low-risk storage path to the pager in this slice to prove the
  boundary without changing file bytes.

The first migrated path should be append-only index leaf run reads/writes or
row payload reads, not catalog publication. Those paths already have storage
unit coverage and exercise cached, active, and durable views without changing
metadata semantics.

## Initial Implementation

The first implementation adds the internal `mylite_storage_pager` wrapper and
migrates row page, row-state page, autoincrement page, BLOB payload page,
free-list, and index leaf page reads plus batched index leaf rebuild writes
through it. The pager currently delegates to the existing fixed-page read/write
helpers, so file bytes, journal selection, active append-buffer reads, and
header-view semantics are unchanged.

Dirty-page tracking, in-place rewrites, page allocation, and WAL/checkpoint
ownership remain follow-on work before maintained B-tree pages can update roots
through the pager.

## Follow-On Design For Maintained Indexes

Maintained index pages should build on the pager, not bypass it:

- catalog index-root records should point to a stable index-root page or
  root-state page;
- row DML should update index pages through pager-owned dirty pages rather than
  publishing a new catalog record per mutation;
- copy-on-write or in-place updates must have one rollback story through the
  pager before page splits are enabled;
- the first maintained index should be limited to current raw fixed-width keys
  used by byte-exact storage probes, leaving collation-sensitive ordering on
  existing MariaDB handler comparisons; and
- full ordered handler cursors should move only after point lookup and
  duplicate-key maintenance are correct and covered.

## Non-Goals

- No SQL-visible behavior change in the pager foundation slice.
- No new public `libmylite` or storage API.
- No WAL, MVCC, lock-manager redesign, or cross-process write concurrency
  claim yet.
- No B-tree page split/merge implementation in the pager foundation slice.
- No row/index free-space reclamation until the pager owns read views and
  rollback for reused pages.

## Compatibility Impact

The pager foundation should be behavior-preserving. MariaDB continues to parse,
optimize, compare key tuples, and call the handler in the same way. Unsupported
server surfaces and current storage-engine routing rules do not change.

## Single-File And Lifecycle Impact

The pager is an internal access layer over the same primary `.mylite` file and
existing MyLite-owned journals. It must not introduce persistent sidecars. Any
future WAL, shared-memory, or lock companion remains a separate documented
slice with lifecycle tests.

## Public API And File-Format Impact

No public API or file-format change is required for the first pager foundation.
Later maintained row-directory or B-tree pages may add page types, but this
slice should prove the access boundary before adding durable structures.

## Storage-Engine Routing Impact

No routing change. The pager applies only after a durable MyLite-routed table is
already using first-party storage.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact should be limited to a small internal
pager struct and wrapper functions. The slice must keep MariaDB-derived code
unchanged unless a call boundary has to pass an existing storage scope.

## Test And Verification Plan

- Add storage unit coverage for the migrated path after it reads/writes through
  the pager.
- Keep active checkpoint, rollback, append-tail, published-leaf, rowset, and
  recovery tests passing.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

- If a storage-smoke path is migrated, also run:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- A pager object exists for internal storage file scopes or active checkpoints.
- One low-risk row or index path reads/writes pages through the pager with no
  file-format or SQL behavior change.
- Current active append-buffer and header-view semantics remain intact.
- Existing storage and relevant storage-smoke tests pass.
- The next maintained-index spec can depend on a pager-owned dirty-page and
  rollback boundary instead of defining its own ad hoc page ownership.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Risks And Unresolved Questions

- A pager abstraction that only wraps calls without simplifying ownership would
  add indirection without value. The first migration must remove duplicate page
  control flow or make a later maintained-index operation clearly simpler.
- In-place dirty page support needs rollback semantics before it is used for
  durable row or index updates. Until then, append-only writes remain the safe
  mutation path.
- Cross-process readers and writers still need the later WAL/checkpoint and
  lock-manager design before full write concurrency can be claimed.
