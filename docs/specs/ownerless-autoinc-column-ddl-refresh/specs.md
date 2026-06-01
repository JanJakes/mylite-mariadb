# Ownerless AUTO_INCREMENT Column DDL Refresh

## Problem

Ownerless AUTO_INCREMENT coverage already proves cross-process implicit insert
reservation and `ALTER TABLE ... AUTO_INCREMENT = N` high-watermark refresh.
One rebuild-style allocation case remains: adding a new `AUTO_INCREMENT`
column to an existing InnoDB table while another ownerless peer already has the
old table definition open.

MariaDB rebuilds the table for `ADD COLUMN ... AUTO_INCREMENT`, assigns values
to existing rows, and installs the new primary key and AUTO_INCREMENT state.
MyLite must prove that an already-open ownerless peer refreshes the rebuilt
dictionary and AUTO_INCREMENT high watermark before its next implicit insert.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` counts
  `AUTO_INCREMENT` columns, rejects multiple auto columns, and verifies the
  target engine supports them.
- `mariadb/sql/sql_table.cc:online_alter_check_supported()` marks
  `ADD COLUMN ... AUTO_INCREMENT` as not online, so the operation follows a
  table rebuild path instead of the instant/in-place column refresh cases.
- `mariadb/storage/innobase/handler/handler0alter.cc` tracks the added
  AUTO_INCREMENT column position as `add_autoinc` in
  `ha_innobase_inplace_ctx` and uses an `ib_sequence_t` while rebuilding rows.
- MyLite's ownerless AUTO_INCREMENT registry stores a monotonic high watermark
  by InnoDB table id, and ownerless statement refresh handles dictionary
  generation changes before the next statement on an already-open peer.

## Scope And Non-Goals

In scope:

- Add focused ownerless SQL coverage for adding an `AUTO_INCREMENT PRIMARY KEY`
  column to a non-empty InnoDB table from one process while another ownerless
  process remains open on the old definition.
- Verify the already-open peer sees the new column metadata and receives the
  next implicit ID after the rebuilt rows.
- Verify final state through ownerless reopen, native exclusive reopen, and
  forced `.shm` rebuild, then insert one more row after rebuild.

Out of scope:

- Full AUTO_INCREMENT DDL matrix coverage.
- Partitioned AUTO_INCREMENT tables, which remain rejected in ownerless mode by
  the partition policy.
- External randomized DDL/RQG oracles.
- Changes to MariaDB native AUTO_INCREMENT semantics.

## Design

Add an `auto-inc-column-ddl` ownerless SQL selector:

1. Create a table without an AUTO_INCREMENT column and insert two rows.
2. Open one ownerless peer and query the old table shape.
3. In a child ownerless process, run
   `ALTER TABLE ... ADD COLUMN id INT NOT NULL AUTO_INCREMENT PRIMARY KEY FIRST`.
4. On the already-open peer, query `information_schema.columns`, verify the new
   `auto_increment` column, and insert a row without specifying `id`.
5. Verify the new row receives `id = 3`, proving the rebuilt table's row count
   and high watermark refreshed before the peer insert.
6. Verify the final rows and metadata through ownerless/native reopen before
   and after deleting `concurrency/mylite-concurrency.shm`.
7. Insert one more ownerless row after forced `.shm` rebuild and verify
   `id = 4`.

The implementation does not add a new ownerless code path unless the focused
test exposes a bug. It relies on the existing dictionary-generation refresh and
AUTO_INCREMENT registry seeding paths.

## Compatibility Impact

This broadens the ownerless AUTO_INCREMENT partial claim from table-option
high-watermark changes to one rebuild-style column DDL class. SQL semantics
remain inherited from MariaDB; the slice proves MyLite's ownerless refresh and
registry layers preserve those semantics across processes for this case.

## Directory And Lifecycle Impact

No directory layout changes. The DDL rebuild remains native InnoDB state inside
the MyLite database directory and uses the existing ownerless `.shm`, WAL, and
checkpoint files.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. The test exercises a rebuild
path that can rewrite the table's clustered index and dictionary metadata, then
verifies the resulting `.frm`/`.ibd` state through ownerless and native reopen.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `auto-inc`, `auto-inc-ddl`, and `auto-inc-column-ddl` selectors.
- Build and run the same focused selectors in `ownerless-test-hooks`.
- Run adjacent ownerless DDL/allocation selectors and ownerless stress.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer sees the newly added AUTO_INCREMENT column.
- The peer's first implicit insert after the rebuild receives `id = 3`.
- After forced `.shm` rebuild, a later ownerless implicit insert receives
  `id = 4`.
- Ownerless and native exclusive reopen preserve row counts, ID sums, value
  sums, and column metadata.

## Risks And Follow-Up

- The test depends on MariaDB's rebuild assigning existing row IDs starting at
  one for this deterministic small table. If MariaDB changes that behavior, the
  compatibility expectation should be adjusted against the base version.
- Broader AUTO_INCREMENT DDL shapes and external randomized DDL oracles remain
  separate follow-up work.
