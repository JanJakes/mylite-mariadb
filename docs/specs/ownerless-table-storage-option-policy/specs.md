# Ownerless Table Storage Option Policy

## Problem

Ownerless read/write mode now covers many ordinary InnoDB DDL paths, including
row-format and compressed key-block rebuilds. It still lacks recovery and
external-oracle evidence for storage options that alter native file layout,
compression, encryption, or tablespace placement. Those options should fail
explicitly in ownerless mode before MariaDB enters unproven file-lifecycle
paths.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level table options including
  `TABLESPACE ident` and engine-defined options after `CREATE TABLE` and
  `ALTER TABLE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `innodb_table_option_list` registers `PAGE_COMPRESSED`,
  `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, and `ENCRYPTION_KEY_ID` as InnoDB
  table options.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `create_table_info_t::check_table_options()` validates encryption and page
  compression against native InnoDB file-per-table, row-format, key-block, and
  key-management constraints.
- Existing MyLite policy rejects ownerless `DATA DIRECTORY`/`INDEX DIRECTORY`,
  partition DDL, `DISCARD/IMPORT TABLESPACE`, and special-index DDL before
  unproven ownerless file-lifecycle or special-storage paths.

## Scope And Non-Goals

In scope:

- Reject ownerless `CREATE TABLE` and `ALTER TABLE` statements that specify
  `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`,
  `ENCRYPTION_KEY_ID`, or table `TABLESPACE` options.
- Keep ordinary covered ownerless row-format and compressed key-block DDL
  available.
- Add ownerless SQL coverage for create-time and alter-time spellings plus a
  quoted-column and CTAS regression so option policy does not reject ordinary
  identifier or result-column names. Follow-up coverage in
  `ownerless-storage-option-ddl-spellings` also covers idempotent,
  replacement, and temporary create-table spellings.
- Verify rejected statements leave existing rows, metadata, and native `.ibd`
  files intact through ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Supporting InnoDB page compression or table encryption under ownerless
  concurrency.
- General tablespace create/drop/alter support.
- Crash injection inside rejected storage-option DDL.
- External MariaDB/RQG execution for storage-option matrices.

## Design

Add a focused SQL-policy predicate in `packages/libmylite/src/database.cc` that
only runs for ownerless read/write handles and scans top-level `CREATE TABLE` /
`ALTER TABLE` tokens after the first `TABLE` keyword. If it finds one of the
unproven engine-defined storage options with MariaDB's required `=` form, or a
top-level table `TABLESPACE` option, `reject_unsupported_sql_policy()` returns
`MYLITE_ERROR` before MariaDB execution.

The policy intentionally does not reject `ROW_FORMAT` or `KEY_BLOCK_SIZE`
because ownerless coverage now includes representative dynamic, compressed, and
focused key-block paths. Backtick-quoted identifiers are not identifier tokens
in the existing policy tokenizer, so quoted column names such as
`` `encrypted` `` remain valid. `CREATE TABLE ... AS SELECT` result aliases
using these words also remain valid because the scan stops at CTAS boundaries.

## Compatibility Impact

Ownerless read/write mode becomes more explicit: unproven table storage-option
DDL fails with a MyLite policy error instead of entering MariaDB native
compression, encryption, or tablespace-option code. Ordinary exclusive
read/write behavior is unchanged.

## Directory And Lifecycle Impact

No directory layout changes. The slice prevents ownerless mode from creating or
rewriting native table files through unproven storage-option paths until their
file-lifecycle and recovery behavior has dedicated coverage.

## Native Storage Impact

No native storage format changes. Native InnoDB page compression, table
encryption, and table `TABLESPACE` behavior remain MariaDB-owned but
unsupported by ownerless read/write mode for now.

## Public API Impact

No public API changes. Rejections use existing `MYLITE_ERROR` diagnostics.

## Binary Size Impact

No production dependency or measurable binary-size impact beyond a small SQL
policy predicate and focused tests.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `table-storage-option-policy`.
- Run adjacent ownerless policy selectors: `table-directory-policy`,
  `partition-policy`, `tablespace-policy`, and `compressed-row-format-ddl`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless SQL label, hook ownerless SQL subset, ownerless
  stress, `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- Ownerless read/write mode rejects page-compression, page-compression-level,
  encryption, encryption-key, and table `TABLESPACE` options for create-time
  and alter-time DDL; the follow-up spelling coverage verifies representative
  idempotent, replacement, and temporary create-table forms.
- Quoted identifier names that match those option words remain usable.
- CTAS result aliases that match those option words remain usable.
- Rejected statements leave baseline rows, table metadata, and the baseline
  `.ibd` file intact across ownerless/native reopen and forced `.shm` rebuild.
- Compatibility docs state the unsupported ownerless storage-option surface and
  keep broader compression, encryption, crash, and external-oracle support as
  planned work.

## Risks And Follow-Up

- The policy is intentionally conservative. Supporting page-compressed or
  encrypted ownerless tables requires a separate native recovery and
  compatibility slice.
- General tablespace-management SQL beyond create-time table options remains a
  separate unsupported-surface audit.
- Idempotent, replacement, and temporary create-table storage-option spellings
  are covered by `ownerless-storage-option-ddl-spellings`.
- External MariaDB/RQG storage-option stress remains planned.
