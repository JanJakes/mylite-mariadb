# Ownerless Live Compressed Key-Block Rebuild Recovery

## Problem Statement

Ownerless compressed row-format crash coverage already proves no-live recovery
for `ALTER TABLE ... ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`, `2`, `4`, and
`16` after MariaDB/InnoDB completes the native compressed copy-style rebuild
but before MyLite publishes ownerless dictionary finish. The previous
`KEY_BLOCK_SIZE=8` live-recovery slice proved the representative compressed
boundary can finish the dead dictionary generation while another ownerless peer
remains live and defer marker drain to the final no-live close.

The remaining compressed key-block gap is to apply the same live-peer recovery
proof to the `1`, `2`, `4`, and `16` page-size variants without broadening to
other ALTER forms.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5904` through
  `mariadb/sql/sql_yacc.yy:5907` parses `KEY_BLOCK_SIZE`, marks
  `HA_CREATE_USED_KEY_BLOCK_SIZE`, and stores the requested key-block size.
- `mariadb/sql/sql_table.cc:11112` through
  `mariadb/sql/sql_table.cc:11126` marks explicit row type ALTERs with
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1537` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11557` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11608` accepts key-block
  values `1`, `2`, `4`, `8`, and `16` under strict validation and requires
  file-per-table for `KEY_BLOCK_SIZE`.
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

Add distinct recovery kinds for:

- `ALTER TABLE <schema>.<table> ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`
- `ALTER TABLE <schema>.<table> ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=2`
- `ALTER TABLE <schema>.<table> ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4`
- `ALTER TABLE <schema>.<table> ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16`

The existing `KEY_BLOCK_SIZE=8` kind remains unchanged. A shared exact
classifier accepts only:

- `ALTER TABLE`;
- a qualified `schema.table` identifier;
- `ROW_FORMAT = COMPRESSED`;
- `KEY_BLOCK_SIZE = 1`, `2`, `4`, `8`, or `16`;
- optional trailing semicolons.

It does not accept omitted key-block size, other row formats,
partition/tablespace options, unqualified table names, or any
non-MariaDB-accepted key-block value. The exact
`ALGORITHM=COPY, LOCK=EXCLUSIVE` tail is covered separately by
`docs/specs/ownerless-compressed-key-block-copy-lock-live-recovery/specs.md`;
broader extra ALTER clauses and option matrices remain out of scope.

The parameterized compressed key-block crash runner changes from a live-peer
`MYLITE_BUSY` probe to a successful live ownerless opener while the original
peer remains held. The opener must:

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
exclusive reopen after rebuild, and native ZBLOB page evidence for each
requested key-block size.

## Scope And Non-Goals

In scope:

- exact `ALTER TABLE schema.table ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1`,
  `2`, `4`, and `16` live-peer dictionary recovery after a writer dies at
  `dictionary-before-finish`;
- compressed row-format metadata, retained prepared BLOB rows, payload bytes,
  first-byte aggregates, post-recovery writes, and final ZBLOB evidence for
  each covered key-block size;
- marker retention while another ownerless peer remains live;
- final no-live marker-drain proof.

Out of scope:

- compressed row-format without explicit key-block size;
- dynamic, compact, redundant, page-compressed, encrypted, partitioned,
  external-directory, tablespace import/discard, and multi-action ALTER
  variants;
- `ALGORITHM`/`LOCK` option matrices beyond the exact
  `ALGORITHM=COPY, LOCK=EXCLUSIVE` follow-up slice;
- broader charset, primary-key, foreign-key, CHECK, generated-column, column,
  schema, view, or trigger rebuild classes;
- SQL-level table-lock callback reachability;
- external MariaDB/RQG stress.

## Compatibility Impact

No new SQL syntax or public API surface is enabled. The slice strengthens the
ownerless concurrency guarantee for the already supported compressed
key-block crash boundaries by replacing no-live-only cleanup with live-peer
dictionary recovery for exact statement shapes that already have metadata,
marker, and no-live recovery evidence.

## DDL Metadata Routing Impact

The dictionary recovery protocol gains distinct recovery kinds for compressed
key-block `1`, `2`, `4`, and `16`. Dead-owner cleanup may finish an active
dictionary generation with a kind only when the dead owner marked the same
kind. Mismatched recovery kinds remain errors in primitive coverage.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable application state and
ownerless coordination files remain inside the MyLite database directory:

- `datadir/app/ownerless_compressed_row_format_kb1.frm`
- `datadir/app/ownerless_compressed_row_format_kb1.ibd`
- `datadir/app/ownerless_compressed_row_format_kb2.frm`
- `datadir/app/ownerless_compressed_row_format_kb2.ibd`
- `datadir/app/ownerless_compressed_row_format_kb4.frm`
- `datadir/app/ownerless_compressed_row_format_kb4.ibd`
- `datadir/app/ownerless_compressed_row_format_kb16.frm`
- `datadir/app/ownerless_compressed_row_format_kb16.ibd`
- `concurrency/mylite-concurrency.ckpt`
- `concurrency/mylite-concurrency.shm`

Live recovery does not drain native file-operation checkpoint evidence while
another ownerless peer is live. The existing final no-live close path remains
responsible for native checkpoint proof and marker clearing.

## Native Storage Impact

MariaDB/InnoDB owns the compressed copy-style rebuild, compressed native page
format, native file swap, and ZBLOB page layout. MyLite does not rewrite table
contents or compressed metadata. The slice only permits ownerless dictionary
publication around the already-finished native compressed rebuilds.

## Public API, Build, Size, License, And Dependencies

No public API, directory layout, dependency, license, or production binary-size
impact is expected. The code adds four internal recovery constants, one shared
exact SQL classifier, and focused test coverage.

## Test And Verification Plan

- Build hook targets:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`.
- Run primitive coverage:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`.
- Run focused hook selectors:
  `dictionary-compressed-row-format-key-block-1-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-2-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`, and
  `dictionary-compressed-row-format-key-block-16-file-op-marker-crash`.
- Run adjacent `dictionary-compressed-row-format-file-op-marker-crash` to
  guard the existing `KEY_BLOCK_SIZE=8` kind.
- Run the registered hook CTest file-op marker subset.
- Build production embedded targets:
  `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`.
- Run production primitive and adjacent ownerless SQL selectors, including
  `native-file-op-marker-drain`,
  `dictionary-compressed-row-format-key-block-1-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-2-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-16-file-op-marker-crash`, and
  `compressed-row-format-key-block-ddl`.
- Run the focused ownerless DDL stress CTest.
- Run `format-check`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Verification Results

Completed locally:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
- Hook selectors:
  `dictionary-compressed-row-format-key-block-1-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-2-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-16-file-op-marker-crash`, and
  `dictionary-compressed-row-format-file-op-marker-crash`
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-primitives|ownerless-native-file-op-marker-drain|ownerless-dictionary-(rename|create-like|ctas|create-or-replace-like|create-or-replace-ctas|truncate|drop|force-rebuild|row-format|compressed-row-format|compressed-row-format-key-block-1|compressed-row-format-key-block-2|compressed-row-format-key-block|compressed-row-format-key-block-16)-file-op-marker-crash)$'
  --output-on-failure` passed `15/15`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
- Production selectors:
  `native-file-op-marker-drain`,
  `dictionary-compressed-row-format-key-block-1-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-2-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`,
  `dictionary-compressed-row-format-key-block-16-file-op-marker-crash`, and
  `compressed-row-format-key-block-ddl`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed `1/1` on rerun. The first attempt aborted after concurrent DDL stress
  workers hit `MYLITE_BUSY` on the ownerless dictionary statement lock; it left
  `/tmp/mylite-ownerless-sql.JCd5rt`, which was removed before the passing
  rerun.

## Acceptance Criteria

- The focused compressed key-block crash selectors reach the hook and do not
  hang.
- Each killed writer leaves durable native file-operation checkpoint evidence
  before live recovery.
- A new ownerless read/write opener succeeds while the original peer is still
  live for each key-block size.
- The live opener observes compressed row-format metadata, retained prepared
  BLOB rows, retained payload bytes, and retained first-byte aggregates.
- The marker remains set after the live opener closes while another peer is
  still live.
- After the peer exits, no-live ownerless recovery supports a post-recovery
  prepared BLOB insert and drains the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  observe the recovered compressed row-format state and native ZBLOB evidence
  for each covered key-block size.

## Risks And Unresolved Questions

- This exact classifier deliberately does not generalize to all compressed
  ALTER forms. The exact copy-lock tail is covered by the follow-up
  `ownerless-compressed-key-block-copy-lock-live-recovery` slice.
- Broader native redo/checkpoint reconciliation and external randomized stress
  remain ownerless completion gates.
