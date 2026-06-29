# Ownerless Row Format Copy Lock Live Recovery

## Problem Statement

Ownerless row-format live recovery covered the plain rebuild spelling:

```sql
ALTER TABLE schema.table ROW_FORMAT=DYNAMIC
```

Applications can also request explicit table-copy semantics and an exclusive
ALTER lock:

```sql
ALTER TABLE schema.table ROW_FORMAT=DYNAMIC, ALGORITHM=COPY, LOCK=EXCLUSIVE
```

Before this slice, MyLite's ownerless dictionary classifier recognized only the
plain row-format rebuild. The explicit copy/lock spelling could complete native
InnoDB rebuild work and then be killed before ownerless dictionary finish
without focused live-peer recovery evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5844-5847` parses `ROW_FORMAT [=] row_types` as a
  table option and sets `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_yacc.yy:8150-8165` admits table options,
  `alter_algorithm_option`, and `alter_lock_option` in the ALTER list, so the
  explicit row-format/copy/lock order is normal MariaDB grammar.
- `mariadb/sql/sql_table.cc:11117-11126` records explicit ALTER row type as
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_table.cc:11426-11462` forces copy-algorithm execution when
  the ALTER operation cannot use non-copy forms.
- `mariadb/storage/innobase/handler/handler0alter.cc:87-97` includes
  `ALTER_OPTIONS` in InnoDB rebuild-sensitive operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:1537-1557` requires a
  rebuild when `ROW_FORMAT` or `KEY_BLOCK_SIZE` is specified.

## Scope And Non-Goals

In scope:

- Accept schema-qualified
  `ALTER TABLE schema.table ROW_FORMAT=DYNAMIC, ALGORITHM=COPY, LOCK=EXCLUSIVE`
  as the same recoverable row-format dictionary kind used by the plain
  `ROW_FORMAT=DYNAMIC` rebuild.
- Preserve the existing plain row-format classifier behavior.
- Add focused unsafe hook CTest coverage that kills the writer at
  `dictionary-before-finish` while another ownerless peer remains live.
- Verify row-format metadata, rows, native file-operation marker retention
  while live, final no-live drain, ownerless/native reopen, and forced `.shm`
  rebuild.

Out of scope:

- `ROW_FORMAT=COMPACT`, `ROW_FORMAT=REDUNDANT`, compressed key-block variants,
  partitioned tables, temporary tables, storage-option paths, and import/discard
  tablespace paths.
- Other `ALGORITHM`/`LOCK` values, omitted commas, or option order matrices.
- External randomized MariaDB/RQG stress.

## Design

Extend `ownerless_alter_table_row_format_dynamic_recovery_statement()` from one
exact tail shape to two:

- end of statement after `ROW_FORMAT=DYNAMIC`;
- `, ALGORITHM=COPY, LOCK=EXCLUSIVE` followed only by optional semicolons.

The explicit copy/lock spelling returns
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC`, reusing
the live-peer row-format recovery path and native file-operation marker drain
policy. The exact copy/exclusive tail parser is shared with the force and
same-engine rebuild classifiers so these bounded ALTER rebuild spellings remain
consistent.

## Compatibility Impact

SQL behavior remains MariaDB-owned. MyLite only broadens ownerless crash
classification after MariaDB has successfully completed this bounded row-format
rebuild and the writer dies before ownerless dictionary finish.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. MariaDB/InnoDB remains the
authority for the rebuilt `.frm` and `.ibd` files in the MyLite database
directory. MyLite records the completed DDL generation as recoverable
ownerless dictionary state and leaves native file-operation marker draining to
the existing no-live checkpoint path.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds a bounded classifier tail, one unsafe hook selector,
CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `dictionary-row-format-copy-lock-crash` directly and through CTest.
- Run adjacent row-format, same-engine, force, and compressed rebuild crash
  selectors.
- Run production `mylite` build, format, CI production-build guard, and
  `git diff --check`.

## Acceptance Criteria

- Explicit `ROW_FORMAT=DYNAMIC, ALGORITHM=COPY, LOCK=EXCLUSIVE` is marked as
  recoverable row-format DDL before dictionary finish.
- A killed writer at `dictionary-before-finish` recovers while a peer remains
  live.
- Rebuilt row-format metadata and table rows survive live recovery, final
  no-live drain, ownerless/native reopen, and forced `.shm` rebuild.
- The native file-operation marker remains durable while the peer is live and
  drains after final no-live recovery.

## Risks And Follow-Up

- This remains a token-level classifier for an exact MariaDB-compatible
  spelling, not a general ALTER parser.
- Compressed key-block and broader algorithm/lock option matrices remain
  planned separately.

## Supersession Notes

- The later `ownerless-compact-row-format-live-recovery` and
  `ownerless-redundant-row-format-live-recovery` slices add focused live-peer
  crash recovery for `ROW_FORMAT=COMPACT` and `ROW_FORMAT=REDUNDANT`.
