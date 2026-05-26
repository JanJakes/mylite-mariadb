# Maintained Root Dirty Page Buffer

## Goal

Reduce the per-row cost of inserting into fresh maintained index roots without
changing SQL-visible behavior or the durable storage format. Maintained-root
page rewrites should be staged in the active storage checkpoint and flushed in
batch at the correct commit boundary instead of synchronously rewriting the
same root page for every inserted row.

## Non-Goals

- No row-page packing or row-id redesign. Row ids still map to row page ids.
- No general WAL, lock-manager, or write-concurrency redesign.
- No maintained branch-root buffering beyond the existing pager write paths.
- No change to engine routing, including routed `ENGINE=InnoDB` tables.
- No new public `libmylite` API or durable page format.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_insert.cc::mysql_insert()` iterates accepted INSERT rows and
  calls `Write_record::write_record()`.
- `mariadb/sql/sql_insert.cc::Write_record::single_insert()` calls
  `table->file->ha_write_row(table->record[0])` for ordinary inserts.
- `mariadb/sql/handler.cc::handler::ha_write_row()` marks the transaction as
  read-write, records handler statistics, and dispatches to the storage engine
  `write_row()` implementation.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` prepares the
  row payload and checked key entries, then calls
  `mylite_storage_append_row_with_index_entries()` for durable file-backed
  tables.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  plans eligible maintained-root inserts, protects their root page ids in the
  statement or recovery journal, writes row payload pages, rewrites maintained
  root pages, writes fallback index-entry pages when needed, and publishes the
  header.
- `packages/mylite-storage/src/storage.c::write_maintained_index_root_inserts()`
  reads and writes each planned single-page root through the pager on every
  append.
- `packages/mylite-storage/src/storage.c::pager_write_page()` captures a
  dirty-page undo and immediately writes the page through `write_page_at()`.
- `packages/mylite-storage/src/storage.c::read_page_at()` already serves active
  statement headers, read-statement snapshots, transaction-journal snapshots,
  and active append-buffer pages before falling back to the primary file.
- `packages/mylite-storage/src/storage.c::mylite_storage_commit_statement()`
  flushes active append pages before publishing the top-level checkpoint
  header. Nested commits merge dirty-page undo state into the parent.
- `packages/mylite-storage/src/storage.c::mylite_storage_rollback_statement()`
  restores header/catalog state, dirty-page undo preimages, buffered append
  undo preimages, append-buffer truncation state, and auto-increment state
  before truncating the file to the checkpoint page count.
- Official MariaDB docs: the embedded interface and storage-engine overview
  describe the `libmysqld` and storage-engine foundation; this slice depends on
  MariaDB source control flow rather than a new documented SQL semantic.

The fresh-root slice measured prepared insert regressions after initial table
creation began publishing empty maintained roots: `prepared-insert-components`
reported the insert step at `160.537 us/op` for 1,000 rows and `1450.113 us/op`
for 10,000 rows, with the commit component at `1031.048 ms` for the larger run.

## Compatibility Impact

SQL behavior should not change. Inserts into routed MyLite tables still become
visible through maintained-root exact and full index readers in the same
statement/transaction. Duplicate-key checks, row rollback, savepoint rollback,
transaction rollback, and close/reopen persistence must continue to observe the
same index contents.

`docs/COMPATIBILITY.md` should not need a new support claim unless tests add a
new compatibility surface. This is a storage performance and lifecycle change
under already documented partial index support.

## Design

Add a statement-owned dirty page buffer for maintained-root insert rewrites.
The primitive stays in storage checkpoint ownership, but the buffering decision
is intentionally narrow: generic pager writes keep their immediate-write
behavior so catalog, free-list, branch-root, update, and delete paths preserve
their existing rollback and physical-byte assumptions.

Implementation boundary:

- Extend `mylite_storage_statement` with a dirty-page buffer keyed by page id.
- Keep each buffered page as a full `MYLITE_STORAGE_FORMAT_PAGE_SIZE` image.
- Route `write_maintained_index_root_inserts()` through a buffered maintained
  root helper that first captures the existing dirty-page undo, then stores the
  replacement page in the active statement's dirty buffer when the page is an
  existing non-header maintained-root page.
- Leave ordinary `pager_write_page()` on the immediate `write_page_at()` path.
  If an immediate pager write touches a page that has a buffered image in the
  active statement chain, discard the buffered image after the durable write
  succeeds so later reads do not see stale root bytes.
- Teach `read_page_at()` to return the nearest active dirty-buffered page before
  checking the append buffer or the primary file. This preserves repeated root
  inserts in one transaction and nested statement visibility.
- Flush buffered append pages and then dirty maintained-root pages before
  publishing the top-level checkpoint header. Root pages protected by the
  journal must be durable before the committed header points at newly appended
  row or index pages.
- On nested commit, merge child dirty buffered pages into the parent. A child
  update to a page already dirty in the parent replaces the parent buffer image
  while preserving the parent's original undo preimage.
- On rollback, discard the statement dirty buffer after existing dirty-page
  undo restoration. If a page was never flushed, discard is enough; if it was
  flushed before rollback, the existing undo list restores the preimage.
- Keep non-active writes, unsupported page-size shapes, and non-maintained-root
  writes on the immediate `write_page_at()` path.

The buffer should remain bounded. If the active dirty-page set exceeds the
limit, flush the owning statement's dirty buffer and keep using the same
journal/undo machinery. The first slice can use the existing protected-page
journal bound as the initial dirty-page count limit because maintained-root
plans are already bounded by that journal shape.

## Affected MariaDB Subsystems

- MariaDB insert execution reaches MyLite through the existing handler
  `write_row()` path.
- MyLite's handlerton transaction and savepoint hooks continue to own
  statement, savepoint, and transaction checkpoint boundaries.
- No parser, optimizer, table metadata, protocol, replication, binlog, or
  account/authentication surface changes.

## DDL Metadata Routing Impact

No table-definition metadata routing change. Empty maintained roots remain
auxiliary MyLite catalog records created by prior DDL work. This slice changes
only how root page bytes are staged while DML mutates those pages.

## Single-File And Embedded Lifecycle Impact

Durable state remains in one primary `.mylite` file. Existing MyLite journal
companions keep their current lifecycle and protected-page semantics. The dirty
page buffer is process memory only. Commit flushes dirty existing pages before
publishing the header; rollback discards dirty buffered images after restoring
the checkpoint view.

Repeated embedded open/close behavior should not change. A committed checkpoint
must reopen with the buffered root entries present. A rolled-back statement,
savepoint, or transaction must reopen without speculative root entries.

## Public API Or File-Format Impact

No public API change and no durable file-format change. The implementation
adds first-party storage internals only.

## Storage-Engine Routing Impact

No routing policy change. File-backed `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and omitted/default engine tables that route to MyLite continue
to use the same handler and storage paths.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package change.

## Build, Size, And Dependencies

Small first-party C storage code only. No new dependency, generated artifact, or
embedded build-profile change.

## Test Plan

- Add storage unit coverage for repeated inserts into fresh maintained roots in
  one active transaction, verifying:
  - exact lookup sees each newly inserted row before commit,
  - page growth avoids fallback index-entry pages,
  - commit and reopen preserve root entries.
- Add rollback coverage for dirty buffered maintained roots, including
  top-level transaction rollback and existing statement, savepoint, and
  recovery coverage that now exercises buffered insert roots.
- Add a test that crosses the dirty-buffer flush limit, proving flushed pages
  still roll back through dirty-page undo.
- Keep embedded storage-engine coverage for a fresh routed `ENGINE=InnoDB`
  table with maintained roots.
- Run focused storage, embedded, static, and performance checks:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 1000
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Maintained-root insert loops do not synchronously rewrite the same root page
  for every row while an active checkpoint can safely buffer the page.
- Reads in the active statement/transaction see the latest dirty root page.
- Savepoint release merges dirty root pages into the parent checkpoint.
- Statement, savepoint, and transaction rollback restore root visibility and
  durable bytes.
- Top-level commit flushes dirty maintained-root pages before publishing the
  header.
- Prepared insert component timings improve materially versus the fresh-root
  baseline while preserving the existing point-read win.
- Existing storage and embedded storage-engine tests pass.

## Risks And Open Questions

- Stale buffered roots can conflict with later immediate update/delete writes
  in the same checkpoint. The implementation must discard buffered images when
  an immediate pager write succeeds for the same page.
- The protected-page journal bound limits how many existing pages can be dirty
  at once. That is acceptable for this slice because maintained-root plans are
  already bounded, but a later real pager/WAL design should relax it.
- This does not make inserts SQLite-fast by itself. One row page per row and
  branch-root navigation costs remain larger roadmap items.
