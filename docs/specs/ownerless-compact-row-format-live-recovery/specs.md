# Ownerless Compact Row-Format Live Recovery

## Problem Statement

Ownerless row-format crash recovery proves a completed native
`ALTER TABLE ... ROW_FORMAT=DYNAMIC` rebuild can be finished while another
ownerless peer remains live. MariaDB also accepts `ROW_FORMAT=COMPACT`, exposes
it as a distinct InnoDB record format, and requires the same table-rebuild
path for explicit row-format changes. MyLite should not leave that supported
native rebuild target on a less-specific recovery lane.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5844-5848` parses `ROW_FORMAT [=] row_types` and
  marks `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_table.cc:11118-11132` marks explicit ALTER row formats in
  `create_info->used_fields`.
- `mariadb/sql/sql_table.cc:11452-11468` forces table-copy execution when an
  ALTER operation cannot use an in-place path.
- `mariadb/storage/innobase/handler/handler0alter.cc:1541-1557` requires an
  InnoDB rebuild when `ROW_FORMAT` or `KEY_BLOCK_SIZE` is specified.
- `mariadb/storage/innobase/handler/i_s.cc:4377-4383` lists
  `Redundant`, `Compact`, `Compressed`, and `Dynamic` row-format values, and
  `i_s.cc:4447-4455` derives the exposed value from native InnoDB table flags.
- `packages/libmylite/src/database.cc` classifies the focused dynamic
  row-format ALTER as
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC`; that
  numeric recovery kind already drives the generic row-format rebuild
  file-operation recovery lane.

## Scope And Non-Goals

In scope:

- Classify schema-qualified `ALTER TABLE ... ROW_FORMAT=COMPACT` as the same
  ownerless row-format rebuild recovery class used for `ROW_FORMAT=DYNAMIC`.
- Keep the exact optional copy-lock tail behavior unchanged.
- Add hook-build crash coverage that starts from a `ROW_FORMAT=DYNAMIC` InnoDB
  table, kills a `ROW_FORMAT=COMPACT` writer at `dictionary-before-finish`,
  and recovers while another ownerless peer remains live.
- Verify native file-operation marker retention while live, final no-live
  drain, recovered rows, post-recovery writes, ownerless/native reopen, and
  forced `.shm` rebuild.

Out of scope:

- `ROW_FORMAT=REDUNDANT`, compressed/key-block variants, page compression,
  encryption, partition DDL, import/discard tablespace, or general ALTER
  parsing.
- Exhaustive `ALGORITHM`/`LOCK` option matrices beyond the existing shared
  copy/exclusive tail.
- External randomized MariaDB/RQG stress.

## Design

Broaden the focused row-format dictionary classifier from one accepted target
token to two:

- `ROW_FORMAT=DYNAMIC`
- `ROW_FORMAT=COMPACT`

The classifier remains conservative: it still requires a schema-qualified table
identifier, an exact row-format clause, and either statement end or the already
supported exact copy/exclusive tail.

The durable numeric recovery kind remains unchanged. The name contains the
original dynamic target, but the recovery path only needs to know that the
native ALTER completed a file-operation row-format rebuild whose dictionary
finish can be replayed safely while peers are live.

## Compatibility Impact

SQL semantics remain MariaDB-owned. The change only extends MyLite's ownerless
crash classification for a MariaDB-accepted native row-format rebuild after
native SQL execution succeeded and before MyLite published dictionary finish.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. The slice exercises MariaDB's
native `.frm`/`.ibd` updates inside the MyLite database directory, the existing
ownerless recoverable dictionary marker, and the existing native file-operation
checkpoint-needed marker.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The code change is a bounded token-classifier expansion plus focused
test coverage and docs.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector directly:
  `dictionary-row-format-compact-file-op-marker-crash`.
- Run adjacent row-format hook CTests:
  `dictionary-row-format-file-op-marker-crash`,
  `dictionary-row-format-copy-lock-crash`, and
  `dictionary-row-format-compact-file-op-marker-crash`.
- Build the production embedded test target and run the production
  `row-format-ddl` selector plus production-build guard.
- Run ownerless stress, format, and whitespace checks.

## Acceptance Criteria

- A killed `ROW_FORMAT=COMPACT` writer reaches `dictionary-before-finish` and
  recovers while another ownerless peer remains live.
- The native file-operation marker remains set while the peer is live and
  drains after final no-live recovery.
- `information_schema.INNODB_SYS_TABLES` and `information_schema.tables` expose
  `Compact` after recovery.
- Retained rows, post-recovery writes, ownerless/native reopen, and forced
  `.shm` rebuild observe the compact row-format table consistently.

## Risks And Follow-Up

- The recovery kind name still says `DYNAMIC`; renaming it would be a cosmetic
  durable-state compatibility risk because the numeric value is already
  persisted. A later cleanup can add an alias if the naming becomes confusing.
- `ROW_FORMAT=REDUNDANT` is covered by the later
  `ownerless-redundant-row-format-live-recovery` slice.
