# Ownerless Current-Read Boundary Rewind

## Problem

`test_ownerless_independent_table_stress` can fail under repeated production
runs when the reader observes a lower value for a table after previously
observing a higher one. The failing reader is an autocommit ownerless
read/write handle repeatedly running plain `SELECT` statements while peer
writers only increment independent single-row tables.

The observed failure means a current live read can still accept an older
physical page image after the handle's monotonic read LSN has advanced.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/ownerless-monotonic-read-lsn/specs.md` states that current live
  reads may accept current ownerless page images that advance the page, while
  retained lower visible-boundary images must not replace newer user pages.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  allowed `refresh_page_for_write()` to overlay a page-version or native disk
  visible-boundary image when the record/disk boundary commit LSN was visible,
  even if the page image's own `FIL_PAGE_LSN` was lower than the local buffer
  page and the read mode was current live visibility.
- `mariadb/storage/innobase/buf/buf0buf.cc` had the same boundary-commit-LSN
  allowance in the page-read completion overlay path.

That boundary rule is necessary for fresh pages, old snapshot reads, and
visible-boundary refresh, but it is unsafe once the same handle has already
observed the same user page at an equal or newer ownerless commit boundary. A
snapshot-boundary page can carry an older page LSN and older row image than the
current local/native page.

## Design

Keep visible-boundary overlay for fresh pages and non-current visibility, but
make retained user-page overlays monotonic per handle and page:

- Page-log reads now return record metadata flags to the ownerless InnoDB read
  hook. Current retained user-page refresh can distinguish a synthesized
  snapshot-boundary record from a real page image even when both are selected by
  commit LSN.
- Each `mylite_db` handle receives a process-local ownerless page-observation
  token. InnoDB hook state records the highest ownerless commit boundary copied
  for `(token, hook context, space_id, page_no)`.
- Successful local page-version publishes also mark the page observed at that
  commit boundary, so a later refresh in the same handle does not replace a
  freshly local same-LSN page image with an older retained page-log image.
- `refresh_page_for_write()` still accepts a page-version or native disk image
  that is fresh for this handle/page, and still accepts images that advance the
  physical page LSN. It rejects retained user-page lower/equal boundary images
  after the same handle has already copied the same page at an equal or newer
  ownerless commit boundary, and rejects snapshot-boundary records during
  retained current-read refresh of user pages.
- The generic page-read completion overlay applies the same per-page guard
  before using a retained boundary commit LSN or same-LSN different ownerless
  image to replace a user page, and treats flagged snapshot-boundary records as
  retained lower-boundary images.

Current reads still accept real ownerless page images that advance the page
image itself or are selected by current commit LSN, and old snapshot reads still
use lower boundary images when they are fresh for that handle/page.

## Compatibility Impact

No public API, SQL syntax, result-shape, file-format, or directory-layout
behavior changes. The change tightens the internal ownerless read overlay rule
so repeated autocommit reads cannot move backward under concurrent positive
updates.

## Native Storage Impact

The change is limited to ownerless InnoDB refresh and page-read overlay logic.
It does not change page-version WAL format, checkpoint format, or native
tablespace replay.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive because upstream-derived InnoDB code
  changes.
- Rebuild production `mylite_ownerless_cross_process_sql_test`.
- Run repeated production `sql-case test_ownerless_independent_table_stress`
  loops.
- Run adjacent ownerless visibility/current-read selectors.
- Re-run the zombie-liveness and DDL-refresh focused cases from the same
  working tree.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Repeated independent-table stress readers do not observe decreasing table
  values.
- Current live reads still see peer increments under concurrent autocommit
  writers.
- Existing retained-boundary and active-reader coverage remains green.

## Risks And Follow-Up

- The page-log read path now exposes snapshot-boundary and external-lineage
  flags to the ownerless InnoDB hook, but broader overlay policy still uses
  page LSN and commit LSN for ordering.
- This does not solve the broader ownerless performance overhead from
  page-version publication or `/proc` process liveness checks.
- External randomized MariaDB/RQG ownerless stress remains a separate
  completion gap.
