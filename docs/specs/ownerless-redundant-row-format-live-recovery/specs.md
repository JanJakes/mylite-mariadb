# Ownerless Redundant Row-Format Live Recovery

## Problem Statement

Ownerless row-format live recovery now covers `ROW_FORMAT=DYNAMIC`,
`ROW_FORMAT=COMPACT`, and compressed key-block rebuild targets. MariaDB also
accepts `ROW_FORMAT=REDUNDANT`, maps it to a distinct InnoDB record format, and
requires the same native table-rebuild path for explicit row-format ALTER
statements. MyLite should either recover that completed native rebuild while
peers remain live or keep it explicitly outside the current compatibility
claim. This slice adds the focused positive proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5844-5848` parses `ROW_FORMAT [=] row_types` and
  marks `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_table.cc:11118-11132` records explicit ALTER row formats.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541-1557` requires a
  rebuild when `ROW_FORMAT` or `KEY_BLOCK_SIZE` is specified.
- `mariadb/storage/innobase/handler/i_s.cc:4377-4383` exposes
  `Redundant`, `Compact`, `Compressed`, and `Dynamic`, and
  `i_s.cc:4447-4455` derives those values from native InnoDB table flags.
- `packages/libmylite/src/database.cc` already routes uncompressed row-format
  rebuilds through the same durable recovery kind after the compact slice.

## Scope And Non-Goals

In scope:

- Classify schema-qualified `ALTER TABLE ... ROW_FORMAT=REDUNDANT` as the same
  ownerless row-format rebuild recovery class used for `DYNAMIC` and
  `COMPACT`.
- Add hook-build crash coverage that starts from a `ROW_FORMAT=DYNAMIC` table,
  kills a `ROW_FORMAT=REDUNDANT` writer at `dictionary-before-finish`, and
  recovers while another ownerless peer remains live.
- Verify native file-operation marker retention while live, final no-live
  marker drain, row-format metadata, retained rows, post-recovery writes,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Additional compressed key-block targets beyond the existing matrix, page
  compression, encryption, partition DDL, import/discard tablespace, and
  generalized ALTER parsing.
- Exhaustive option-order coverage; the shared copy/exclusive tail remains
  covered by existing dynamic/compressed representatives.
- External randomized MariaDB/RQG stress.

## Design

Extend the focused row-format classifier token set from
`DYNAMIC|COMPACT` to `DYNAMIC|COMPACT|REDUNDANT`. The classifier remains
schema-qualified, exact-token, and conservative about trailing clauses.

The hook test reuses the parameterized row-format crash harness. The new
selector creates the source table as `ROW_FORMAT=DYNAMIC`, executes
`ALTER TABLE app.ownerless_row_format_base ROW_FORMAT=REDUNDANT` under the
existing `dictionary-before-finish` hook, kills the writer, and verifies
`INNODB_SYS_TABLES.ROW_FORMAT = 'Redundant'` plus matching
`information_schema.tables.row_format`.

## Compatibility Impact

SQL behavior remains MariaDB-owned. MyLite only broadens the ownerless recovery
classifier for a native MariaDB row-format rebuild after native execution has
completed and before ownerless dictionary finish is published.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. The slice exercises native
InnoDB `.frm`/`.ibd` metadata and data inside the MyLite database directory,
the existing ownerless dictionary marker, and the existing native
file-operation checkpoint-needed marker.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive build-profile
change.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector directly:
  `dictionary-row-format-redundant-file-op-marker-crash`.
- Run adjacent row-format hook CTests covering dynamic, compact, redundant, and
  copy-lock dynamic variants.
- Build the production embedded test target and run production row-format DDL
  plus production-build guard.
- Run ownerless stress, format, and whitespace checks.

## Acceptance Criteria

- A killed `ROW_FORMAT=REDUNDANT` writer reaches `dictionary-before-finish` and
  recovers while another ownerless peer remains live.
- The native file-operation marker remains set while the peer is live and
  drains after no-live recovery.
- `information_schema.INNODB_SYS_TABLES` and `information_schema.tables` expose
  `Redundant` after recovery.
- Retained rows, post-recovery writes, ownerless/native reopen, and forced
  `.shm` rebuild observe the redundant row-format table consistently.
