# Ownerless Post-DDL Boundary Suppression

## Problem

Native snapshot-boundary synthesis reads an older on-disk InnoDB page image
when a writer publishes a newer page while another process holds an older
page-version pin. The existing guard accepts the native page when its
`FIL_PAGE_LSN` is at or before the pinned snapshot LSN.

That proof is not strong enough immediately after local DDL creates a new
file-per-table tablespace. A newly created `.ibd` can contain pages whose page
LSNs are at or below an older reader's snapshot LSN, but the file did not exist
at that snapshot. Publishing those pages as synthesized boundaries, or
refreshing the local new-space allocation pages from external page-version
state during the same post-DDL window, can feed stale state back into the same
writer and corrupt the live secondary index before no-live replay even starts.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:publish_ownerless_snapshot_boundary_if_needed()`
  snapshots the oldest active page-version pin, marks external-lineage
  consumption, and then tries to synthesize a boundary by reading the current
  native tablespace page at or before the oldest pin LSN.
- `packages/libmylite/src/ownerless_tablespace_replay.cc:mylite_ownerless_tablespace_read_page_at_or_before()`
  resolves a file-per-table `.ibd` by page-0 space id and accepts the requested
  page only when the page header identity and page LSN fit the target.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:refresh_page_for_write()`
  can overlay an external page-version image into a writer's local buffer-pool
  page during conservative write refresh.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::ownerless_space_write_enter()`
  calls `mylite_ownerless_innodb_refresh_external_space_allocation()` before
  writing a tablespace's allocation metadata. That helper already skipped
  active dictionary DDL but not the retained-pin post-DDL conservative-write
  window.
- Direct and prepared ownerless SQL execution already keep
  `ownerless_peer_dictionary_refresh_requires_conservative_write` set after
  local dictionary DDL while an external page-version pin exists. That state is
  the narrow window where native page LSN is insufficient file-lifecycle proof.

## Design

Add an internal InnoDB thread-local statement flag named
`mylite_ownerless_statement_suppress_native_lifecycle_refresh`. Direct and
prepared ownerless execution set it through an RAII scope while executing:

- dictionary DDL statements, and
- statements running while the handle's post-DDL conservative-write flag is
  still active.

The page-publish hook still computes whether an external snapshot pin is
active and still marks
`ownerless_runtime_consumed_external_snapshot_page_version_wal`. It returns
before appending a synthesized native boundary only when the new suppression
flag is set. Normal page-version publication still appends the current page
record, so retained-reader WAL and later conservative reclaim remain safe.

The same statement flag also suppresses
`mylite_ownerless_innodb_refresh_external_space_allocation()`. During local
post-DDL writes, InnoDB's allocation pages for the new file-per-table space are
owned by the local statement sequence; refreshing them from page-version or
disk state chosen under an older retained snapshot can move allocation metadata
backward. The guard is limited to active dictionary DDL and the same post-DDL
conservative-write window.

Ordinary pre-existing table DML keeps native boundary synthesis enabled. This
preserves the existing performance and retention win for the common live
snapshot-pin case where the native page image actually can prove the active
snapshot boundary.

## Compatibility Impact

SQL behavior is unchanged except that the ownerless writer no longer corrupts
newly created indexed file-per-table tables while another process holds an
older repeatable-read snapshot. MySQL/MariaDB-visible DDL and DML semantics
remain MariaDB-native.

## Directory And Lifecycle Impact

No files, public API, or WAL format change. The change narrows when MyLite may
treat a native `.ibd` page as an older snapshot boundary and when InnoDB may
refresh allocation pages from external state; it does not add DDL
file-lifecycle metadata or reconstruct missing created tablespaces.

## Native Storage Impact

Native snapshot-boundary synthesis remains a read-only best-effort path for
pre-existing tablespace pages. It is disabled while the current statement is
inside a local dictionary change or the post-DDL conservative-write window
because page LSN does not prove file existence at the pinned snapshot.
External space-allocation refresh is disabled in the same window for the same
local new-space lifecycle reason.

## Test Plan

- Rebuild the MariaDB embedded archive after the InnoDB hook header/source
  change.
- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run focused SQL case 70,
  `test_ownerless_created_tablespace_replay_keeps_created_space`.
- Run focused SQL case 48,
  `test_ownerless_live_snapshot_pin_synthesizes_page_boundary`, to prove
  ordinary native boundary synthesis still works.
- Run adjacent post-DDL case 65,
  `test_ownerless_rename_create_tablespace_replay_keeps_both_spaces`.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- Case 70 completes the live post-create insert/update sequence without InnoDB
  secondary-index corruption and preserves ownerless/native reopen state before
  and after forced `.shm` rebuild.
- Case 48 still observes synthesized native boundaries for pre-existing pages.
- Case 65 still keeps post-DDL writes conservative while the external pin is
  active.
- The external-lineage retention bit is still set when a suppressed statement
  publishes current page versions under an external pin.

## Risks And Open Questions

- This is a conservative correctness guard, not full DDL file-lifecycle
  metadata. Broader DDL/file lifecycle recovery remains planned.
- It can retain more WAL and skip more allocation refresh during live post-DDL
  reader pressure than a future lifecycle-aware boundary proof would require.
- SQL-level table-lock fault injection remains blocked by callback reachability
  and is unrelated to this fix.
