# Ownerless Compressed Key-Block Copy Lock Live Recovery

## Problem Statement

Ownerless compressed row-format live recovery covers the plain key-block
rebuild spellings:

```sql
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=2
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16
```

Applications can also request explicit table-copy semantics and an exclusive
ALTER lock for the same native compressed rebuild:

```sql
ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>,
  ALGORITHM=COPY, LOCK=EXCLUSIVE
```

Before this slice, MyLite's ownerless dictionary classifier recognized only the
plain compressed key-block spellings. The explicit copy/lock tail could leave a
completed native compressed rebuild outside focused live-peer recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5844-5847` parses `ROW_FORMAT [=] row_types` as a
  table option and sets `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_yacc.yy:5904-5907` parses `KEY_BLOCK_SIZE`, sets
  `HA_CREATE_USED_KEY_BLOCK_SIZE`, and stores the requested key-block size.
- `mariadb/sql/sql_yacc.yy:8150-8165` admits table options,
  `alter_algorithm_option`, and `alter_lock_option` in the ALTER list.
- `mariadb/sql/sql_table.cc:11117-11126` records explicit ALTER row type as
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1537-1557` requires an
  InnoDB rebuild when `ROW_FORMAT` or `KEY_BLOCK_SIZE` is specified.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11557-11608` validates
  key-block sizes `1`, `2`, `4`, `8`, and `16` for InnoDB compressed tables.
- `mariadb/storage/innobase/handler/ha_innodb.cc:12027-12135` maps
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` to compressed native page state.

## Scope And Non-Goals

In scope:

- Accept schema-qualified
  `ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>,
  ALGORITHM=COPY, LOCK=EXCLUSIVE` for key-block sizes `1`, `2`, `4`, `8`, and
  `16` as the same recoverable dictionary kinds used by the plain spellings.
- Preserve existing plain compressed key-block classifier behavior.
- Add focused unsafe hook CTest coverage that kills each copy-lock writer at
  `dictionary-before-finish` while another ownerless peer remains live.
- Verify compressed metadata, retained prepared BLOB rows, native ZBLOB page
  evidence, marker retention while live, final no-live drain, ownerless/native
  reopen, and forced `.shm` rebuild.

Out of scope:

- Compressed row-format without explicit key-block size.
- `ROW_FORMAT=COMPACT`, `ROW_FORMAT=REDUNDANT`, page compression, encryption,
  partitioned tables, temporary tables, storage-option paths, and
  import/discard tablespace paths.
- Other `ALGORITHM`/`LOCK` values, omitted commas, or option order matrices.
- External randomized MariaDB/RQG stress.

## Design

Extend `ownerless_alter_table_compressed_row_format_key_block_recovery_kind()`
from one exact tail shape to two:

- end of statement after `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>`;
- `, ALGORITHM=COPY, LOCK=EXCLUSIVE` followed only by optional semicolons.

The key-block value still maps to the existing distinct recovery kind for
`1`, `2`, `4`, `8`, or `16`. No durable format or enum change is needed. The
exact copy/exclusive tail parser is shared with force, same-engine, and dynamic
row-format rebuild classifiers.

## Compatibility Impact

SQL behavior remains MariaDB-owned. MyLite only broadens ownerless crash
classification after MariaDB has successfully completed the bounded compressed
key-block rebuild and the writer dies before ownerless dictionary finish.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. MariaDB/InnoDB remains the
authority for rebuilt compressed `.frm` and `.ibd` files in the MyLite database
directory. MyLite records the completed DDL generation as recoverable ownerless
dictionary state and leaves native file-operation marker draining to the
existing no-live checkpoint path.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds a bounded classifier tail, one unsafe hook selector
covering five key-block sizes, CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `dictionary-compressed-row-format-key-block-copy-lock-crash` directly and
  through CTest.
- Run adjacent row-format, compressed row-format, and compressed key-block crash
  selectors.
- Run production `mylite` build, format, CI production-build guard, and
  `git diff --check`.

## Acceptance Criteria

- Explicit compressed key-block copy-lock spellings for `1`, `2`, `4`, `8`, and
  `16` are marked as recoverable before dictionary finish.
- A killed writer at `dictionary-before-finish` recovers while a peer remains
  live for each key-block size.
- Compressed row-format metadata, retained BLOB rows, native ZBLOB page
  evidence, final no-live drain, ownerless/native reopen, and forced `.shm`
  rebuild remain correct.
- The native file-operation marker remains durable while the peer is live and
  drains after final no-live recovery.

## Risks And Follow-Up

- This remains a token-level classifier for exact MariaDB-compatible spellings,
  not a general ALTER parser.
- Broader algorithm/lock option matrices and unsupported storage-option paths
  remain planned separately.
