# Maintained Branch Insert Dirty Page Buffer

## Goal

Reduce prepared insert step cost once maintained fixed-width indexes have
promoted from a single-page root to branch pages. Repeated branch-maintenance
inserts should stage rewrites of existing index leaf and branch pages in the
active storage checkpoint, then flush those dirty page images at the checkpoint
boundary, instead of synchronously rewriting the same branch path on every row.

## Non-Goals

- No generic pager buffering. Catalog, free-list, row-state, update, delete,
  truncate, and copy-rebuild writes keep their existing immediate-write paths.
- No row-page packing or row-id redesign.
- No branch split, merge, refold, or B-tree navigation algorithm change.
- No new public `libmylite` API or durable page format.
- No change to engine routing, including routed `ENGINE=InnoDB` tables.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_insert.cc::mysql_insert()` drives accepted INSERT rows
  through `Write_record::write_record()`.
- `mariadb/sql/sql_insert.cc::Write_record::single_insert()` dispatches each
  ordinary row through `table->file->ha_write_row(table->record[0])`.
- `mariadb/sql/handler.cc::handler::ha_write_row()` marks the handler
  transaction read-write and calls the storage engine `write_row()` method.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` prepares the
  row payload plus checked key entries and calls
  `mylite_storage_append_row_with_index_entries()` for durable MyLite tables.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  plans maintained root and branch index inserts, writes the row payload, then
  applies maintained index root/branch rewrites before publishing the header.
- `packages/mylite-storage/src/storage.c::write_branch_index_root_inserts()`
  dispatches supported branch insert shapes to level-specific helpers for
  single-level, level-`2`, level-`3`, level-`4`, and deeper roots.
- Fitting branch insert helpers update existing leaf and branch pages through
  `pager_write_page()`:
  - `insert_branch_index_leaf_entry()` rewrites the selected leaf and root
    branch pages.
  - `insert_level_two_branch_index_leaf_entry()` rewrites the selected leaf,
    lower branch, and root branch pages.
  - `insert_level_three_branch_index_leaf_entry()` rewrites the selected leaf,
    lower branch, child branch, and root branch pages.
  - `insert_level_four_branch_index_leaf_entry()` and
    `insert_deep_branch_index_leaf_entry()` extend the same pattern through
    deeper branch paths.
- `packages/mylite-storage/src/storage.c::pager_write_page()` currently
  captures dirty-page undo and immediately writes the page to the primary file.
- The previous maintained-root slice added a statement-owned dirty page buffer,
  `read_page_at()` overlay reads, top-level dirty-buffer flushing before header
  publication, nested dirty-buffer merge, rollback discard, and stale-buffer
  discard after immediate writes.

The maintained-root dirty buffer reduced top-level prepared insert commit
publication cost, but prepared insert step time remained high once root pages
promote to branch roots because each row still rewrites the selected branch
path synchronously.

## Compatibility Impact

SQL-visible behavior should not change. Inserts into routed MyLite tables must
remain visible through the current maintained index readers inside the active
statement, savepoint, and transaction. Duplicate-key checks, exact reads,
prefix/full index reads, rollback, savepoint rollback, transaction rollback,
crash recovery, and close/reopen persistence must observe the same rows as the
immediate-write implementation.

This is a storage performance and lifecycle change under existing partial index
support. It should not add a new `docs/COMPATIBILITY.md` claim.

## Design

Rename the maintained-root dirty write helper to a maintained-index insert
write helper and let it buffer existing index root, branch, and leaf pages
only when the caller is a maintained insert path. The buffer primitive remains
statement-owned and bounded exactly as in the maintained-root slice.

Implementation boundary:

- Replace `pager_write_buffered_maintained_root_page()` with a more general
  `pager_write_buffered_maintained_index_page()` helper.
- Broaden the helper's page eligibility check from maintained root pages to
  MyLite index root, branch, and leaf pages.
- Keep the helper opt-in. Only call it from maintained insert paths:
  `write_maintained_index_root_inserts()` and branch insert helpers that
  maintain existing branch/leaf pages.
- Do not route ordinary `pager_write_page()` through the buffer. Immediate
  update/delete, catalog, free-list, snapshot publication, and copy-rebuild
  writes still write synchronously and discard any stale buffered image for the
  touched page after the durable write succeeds.
- Existing pages may be buffered only when `page_id` is below the statement
  checkpoint `page_count` and not already represented by the append buffer.
  Newly appended split or snapshot pages keep the current write path.
- Continue to capture dirty-page undo before storing the first buffered image
  in the current statement, unless the parent statement has a buffered image
  for that page. A parent dirty-page undo without a parent buffered image is
  not enough for child rollback because the child may need to restore the
  parent's current disk view.
- Preserve `read_page_at()` overlay order so active reads see the nearest
  dirty-buffered branch path before read snapshots, append buffers, and the
  primary file.
- Preserve top-level flush ordering: append buffer first, dirty index page
  buffer second, header publication third.
- Preserve nested commit/rollback behavior by reusing existing dirty-buffer
  merge and discard semantics.

## Affected MariaDB Subsystems

- MariaDB insert execution still reaches MyLite through the existing handler
  `write_row()` path.
- MyLite handlerton transaction and savepoint hooks continue to own statement,
  savepoint, and transaction checkpoint boundaries.
- No parser, optimizer, table metadata, protocol, replication, binlog, account,
  or authentication surface changes.

## DDL Metadata Routing Impact

No table-definition metadata change. Existing index-root catalog metadata keeps
identifying maintained root and branch pages. This slice changes only how
existing index page bytes are staged while DML mutates them.

## Single-File And Embedded Lifecycle Impact

Durable state remains in the primary `.mylite` file. The dirty page buffer is
process memory only. Existing MyLite journal companions continue to protect the
original dirty page images for rollback and stale recovery.

Commit must flush buffered existing index pages before publishing a header that
points at new row or index pages. Rollback must restore or discard speculative
branch path changes before truncating uncommitted append pages.

## Public API Or File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

No routing policy change. File-backed `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, and omitted/default engine tables that route to MyLite continue
to use the same storage paths.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package change.

## Build, Size, And Dependencies

Small first-party C storage changes only. No new dependency, generated
artifact, or embedded build-profile change.

## Test Plan

- Add storage coverage for repeated inserts into an existing branch root inside
  one active transaction, verifying:
  - exact lookup sees newly inserted rows before commit,
  - the existing branch root remains physically unchanged until commit,
  - commit and reopen preserve branch-maintained entries.
- Add rollback coverage for branch insert dirty pages, including transaction
  rollback and nested savepoint rollback when the child rewrites a parent
  buffered branch page.
- Keep existing branch split and branch delete/update coverage passing, proving
  non-insert immediate paths still discard stale buffered images.
- Keep embedded storage-engine coverage for routed `ENGINE=InnoDB` maintained
  index behavior.
- Run focused storage, embedded, static, and performance checks:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 1000
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-components 1000 10000
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Maintained branch insert loops no longer synchronously rewrite the same
  existing branch path while an active checkpoint can safely buffer it.
- Reads in the active statement/transaction see the newest dirty leaf and
  branch pages.
- Nested savepoint release merges branch insert dirty pages into the parent
  checkpoint.
- Statement, savepoint, and transaction rollback restore logical visibility and
  durable branch/index page bytes.
- Top-level commit flushes dirty branch insert pages before publishing the
  header.
- Existing branch split/update/delete tests pass without being moved to the
  dirty-buffer path.
- Prepared insert step cost improves materially or the remaining bottleneck is
  measured and recorded.

## Risks And Open Questions

- Buffering more page types increases stale-image risk if an immediate
  update/delete path touches the same page after an insert path buffered it.
  The existing immediate-write discard rule must remain covered by tests.
- The protected-page journal bound still caps the number of existing pages that
  can be dirty at once. Large maintained branch paths may flush mid-checkpoint
  until a real pager/WAL design replaces this bounded helper.
- This does not address one-row-per-page row storage. Even after branch insert
  buffering, row layout remains a likely prepared insert bottleneck.
