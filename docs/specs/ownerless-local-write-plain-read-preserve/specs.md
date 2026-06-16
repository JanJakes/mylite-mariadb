# Ownerless Local-Write Plain-Read Preservation

## Problem Statement

Ownerless page-version reads are enabled for direct and prepared `SELECT`/`WITH`
statements so a handle can read peer-committed pages at a live read LSN. Inside
an explicit InnoDB transaction that already performed a local write, that
plain-read path must not refresh over the transaction's own dirty buffer-pool
pages.

The existing savepoint visibility case exposed a regression: after
`START TRANSACTION` and `UPDATE app.ownerless_sql SET value = 11 WHERE id = 1`,
the child process's immediate `SELECT value ... WHERE id = 1` read the old
committed value. The page-version plain-read refresh was allowed because the
handle did not yet have a local-native read watermark, even though the
transaction state already recorded a local write.

## Source Findings

- `packages/libmylite/src/database.cc`
  `refresh_ownerless_external_pages_before_statement()` can enable
  page-version reads for eligible `SELECT` statements inside explicit
  transactions.
- `packages/libmylite/src/database.cc`
  `update_ownerless_transaction_state_after_successful_sql()` records local
  writes and locking reads for the active explicit transaction.
- `mariadb/storage/innobase/row/row0sel.cc`
  `row_sel_ownerless_current_read_refresh_needed()` suppresses the plain-read
  refresh only when `mylite_ownerless_innodb_statement_plain_read_preserves_local_pages()`
  is true.

## Design

Direct and prepared statement execution now set the ownerless plain-read
`preserve_local_pages` flag when either:

- the handle already has a local-native read LSN, or
- the current explicit transaction has performed a local write or locking read.

This preserves the current transaction's dirty pages while keeping page-version
read pins and peer-visible read boundaries available for eligible transaction
reads. It does not disable ordinary ownerless page-version reads, and it does
not change DML/current-read refresh policy for statements that are not plain
reads.

## Compatibility Impact

No public C API or SQL syntax changes. The behavior matches MariaDB/InnoDB
transaction visibility: a transaction must read its own writes, including
writes that precede a savepoint rollback of later changes.

## Directory And Lifecycle Impact

No directory layout changes. The slice only changes statement-local ownerless
plain-read flags.

## Native Storage Impact

Native InnoDB page formats and redo/undo behavior are unchanged. The change
prevents an ownerless page-version read helper from refreshing over dirty
local buffer-pool pages that belong to the active transaction.

## Test Plan

- Rebuild the production embedded ownerless SQL test target.
- Run direct `sql-case 6`
  (`test_ownerless_savepoint_rollback_is_peer_visible_after_commit`).
- Run the focused `savepoint` selector.
- Run nearby transaction/read ownerless SQL selectors and the focused embedded
  ownerless CTest subset.
- Run production build guards and whitespace checks before commit.
