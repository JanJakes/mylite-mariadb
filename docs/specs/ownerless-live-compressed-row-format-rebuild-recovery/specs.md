# Ownerless Live Compressed Row-Format Rebuild Recovery

## Problem Statement

Ownerless compressed row-format hook coverage already kills an
`ALTER TABLE app.ownerless_compressed_row_format_base ROW_FORMAT=COMPRESSED
KEY_BLOCK_SIZE=8` writer after MariaDB/InnoDB completes the native compressed
copy-style rebuild but before MyLite publishes ownerless dictionary finish.
That coverage preserves compressed metadata, prepared BLOB rows, and native
ZBLOB page evidence through no-live recovery, and the marker slice proves the
killed writer leaves the native file-operation checkpoint-needed marker while a
peer is still live.

The remaining gap for this focused shape is live-peer dictionary recovery. A
new ownerless opener should be able to finish the dead writer's dictionary
generation while an unrelated peer remains live, observe the rebuilt compressed
table, and leave final native marker drain to the existing no-live close path.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5907` parses `KEY_BLOCK_SIZE`, marks
  `HA_CREATE_USED_KEY_BLOCK_SIZE`, and stores the key-block size.
- `mariadb/sql/sql_table.cc:11112` through
  `mariadb/sql/sql_table.cc:11126` marks explicit row type ALTERs with
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1537` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11557` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11608` accepts only
  key-block values `1`, `2`, `4`, `8`, and `16` under strict validation and
  requires file-per-table for `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:12027` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:12135` maps
  `ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=<n>` to native compressed record format
  and compressed page-size state.
- `mariadb/storage/innobase/handler/i_s.cc:4377` through
  `mariadb/storage/innobase/handler/i_s.cc:4383` exposes `Compressed` in
  `INNODB_SYS_TABLES.ROW_FORMAT`, and `mariadb/storage/innobase/handler/i_s.cc:4447`
  through `mariadb/storage/innobase/handler/i_s.cc:4473` derives row format and
  `ZIP_PAGE_SIZE` from native dictionary flags.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` uses the copy-ALTER path for
  rebuilds: it creates the intermediate table around
  `mariadb/sql/sql_table.cc:11817`, copies rows through
  `copy_data_between_tables()` around `mariadb/sql/sql_table.cc:11943`, and
  swaps native files around `mariadb/sql/sql_table.cc:12135` through
  `mariadb/sql/sql_table.cc:12170`.

## Design

Add a new exact recovery kind for:

```sql
ALTER TABLE <schema>.<table> ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8
```

The classifier accepts only:

- `ALTER TABLE`;
- a qualified `schema.table` identifier;
- `ROW_FORMAT = COMPRESSED`;
- `KEY_BLOCK_SIZE = 8`;
- optional trailing semicolons.

It does not accept other key-block sizes, omitted key-block size, other row
formats, extra ALTER clauses, partition/tablespace options, or unqualified
table names. The 1/2/4/16 key-block crash selectors remain conservative until
separate live-recovery slices widen them.

The focused compressed crash runner changes from a live-peer `MYLITE_BUSY`
probe to a live ownerless opener while the old peer remains held. The opener
must:

- recover the dead writer's dictionary generation;
- observe `INNODB_SYS_TABLES.ROW_FORMAT = 'Compressed'`;
- observe `information_schema.tables.row_format = 'Compressed'`;
- read the two retained prepared BLOB rows, payload lengths, and first-byte
  aggregates;
- close while the native file-operation checkpoint marker remains set because
  another peer is still live.

After the held peer exits, the existing no-live opener verifies the same
compressed state, inserts another prepared BLOB row, closes, and requires the
native marker to be drained. Existing reopen helpers continue to prove ownerless
reopen, ordinary native exclusive reopen, forced `.shm` rebuild, ordinary native
exclusive reopen after rebuild, and native ZBLOB page evidence.

## Scope And Non-Goals

In scope:

- exact `ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8`
  live-peer dictionary recovery after a writer dies at `dictionary-before-finish`;
- compressed row-format metadata, retained prepared BLOB rows, payload bytes,
  first-byte aggregates, post-recovery writes, and final ZBLOB evidence;
- marker retention while another ownerless peer remains live;
- final no-live marker-drain proof.

Out of scope:

- `KEY_BLOCK_SIZE=1`, `2`, `4`, and `16` live-peer recovery;
- compressed row-format without explicit key-block size;
- dynamic, compact, redundant, page-compressed, encrypted, partitioned,
  external-directory, tablespace import/discard, and multi-action ALTER
  variants;
- broader charset, primary-key, foreign-key, CHECK, generated-column, column,
  schema, view, or trigger rebuild classes;
- SQL-level table-lock callback reachability;
- external MariaDB/RQG stress.

## Compatibility Impact

No new SQL syntax or public API surface is enabled. The slice strengthens the
ownerless concurrency guarantee for the already supported
`ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8` crash boundary by replacing
no-live-only cleanup with live-peer dictionary recovery for one exact statement
shape.

Other compressed key-block sizes remain partial until separate live-recovery
evidence is added.

## DDL Metadata Routing Impact

The dictionary recovery protocol gains a distinct
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_COMPRESSED_ROW_FORMAT_KEY_BLOCK_8`
kind. Dead-owner cleanup may finish an active dictionary generation with that
kind only when the dead owner marked the same kind. Mismatched recovery kinds
remain errors in primitive coverage.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable application state and
ownerless coordination files remain inside the MyLite database directory:

- `datadir/app/ownerless_compressed_row_format_base.frm`,
- `datadir/app/ownerless_compressed_row_format_base.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

Live recovery does not drain native file-operation checkpoint evidence while
another ownerless peer is live. The existing final no-live close path remains
responsible for native checkpoint proof and marker clearing.

## Native Storage Impact

MariaDB/InnoDB owns the compressed copy-style rebuild, compressed native page
format, native file swap, and ZBLOB page layout. MyLite does not rewrite table
contents or compressed metadata. The slice only permits ownerless dictionary
publication around the already-finished native compressed rebuild.

## Public API, Build, Size, License, And Dependencies

No public API, directory layout, dependency, license, or production binary-size
impact is expected. The code adds one internal recovery constant, one exact SQL
classifier, and focused test coverage.

## Test And Verification Plan

- Build hook targets:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`.
- Run primitive coverage:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`.
- Run focused hook selectors:
  `dictionary-compressed-row-format-file-op-marker-crash`,
  `dictionary-compressed-row-format-crash`,
  `dictionary-row-format-file-op-marker-crash`,
  `dictionary-force-rebuild-file-op-marker-crash`, and
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`.
- Run the registered hook CTest file-op marker subset.
- Build production embedded targets:
  `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`.
- Run production primitive and adjacent ownerless SQL selectors, including
  `native-file-op-marker-drain`,
  `dictionary-compressed-row-format-file-op-marker-crash`,
  `compressed-row-format-ddl`, and `compressed-row-format-tablespace-replay`.
- Run the focused ownerless DDL stress CTest.
- Run `format-check`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Verification Results

Completed locally:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
- Hook selectors:
  `dictionary-compressed-row-format-file-op-marker-crash`,
  `dictionary-compressed-row-format-crash`,
  `dictionary-row-format-file-op-marker-crash`,
  `dictionary-force-rebuild-file-op-marker-crash`, and
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-primitives|ownerless-native-file-op-marker-drain|ownerless-dictionary-(rename|create-like|ctas|create-or-replace-like|create-or-replace-ctas|truncate|drop|force-rebuild|row-format|compressed-row-format|compressed-row-format-key-block-1|compressed-row-format-key-block-2|compressed-row-format-key-block|compressed-row-format-key-block-16)-file-op-marker-crash)$'
  --output-on-failure` passed `15/15`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
- Production selectors:
  `native-file-op-marker-drain`,
  `dictionary-compressed-row-format-file-op-marker-crash`,
  `compressed-row-format-ddl`, and
  `compressed-row-format-tablespace-replay`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed `1/1`

## Acceptance Criteria

- The focused compressed row-format crash selectors reach the hook and do not
  hang.
- The killed compressed row-format writer leaves durable native file-operation
  checkpoint evidence before live recovery.
- A new ownerless read/write opener succeeds while the original peer is still
  live.
- The live opener observes compressed row-format metadata, retained prepared
  BLOB rows, retained payload bytes, and retained first-byte aggregates.
- The marker remains set after the live opener closes while another peer is
  still live.
- After the peer exits, no-live ownerless recovery supports a post-recovery
  prepared BLOB insert and drains the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  observe the recovered compressed row-format state and native ZBLOB evidence.

## Risks And Unresolved Questions

- This exact classifier deliberately does not generalize to other key-block
  sizes or all compressed ALTER forms.
- The key-block 1/2/4/16 crash selectors still need separate live-recovery
  evidence.
- Broader native redo/checkpoint reconciliation and external randomized stress
  remain ownerless completion gates.
