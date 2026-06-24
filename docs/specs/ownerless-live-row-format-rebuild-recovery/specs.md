# Ownerless Live Row-Format Rebuild Recovery

## Problem Statement

Ownerless hook coverage already kills an
`ALTER TABLE app.ownerless_row_format_base ROW_FORMAT=DYNAMIC` writer after
MariaDB/InnoDB completes the native copy-style row-format rebuild but before
MyLite publishes ownerless dictionary finish. The original recovery proof kept
live peers conservative: a new ownerless opener returned `MYLITE_BUSY` until
the old live peer exited and no-live recovery rebuilt volatile state.

This slice narrows that gap for one exact supported DDL shape. A dead writer's
recoverable dictionary marker should let another ownerless process finish the
dictionary generation while a separate peer is still live, observe the rebuilt
dynamic row-format table, and leave native file-operation checkpoint drain to
the existing final no-live close path.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:11112` through
  `mariadb/sql/sql_table.cc:11126` preserves the old row type for ALTER
  statements without an explicit row type and marks explicit row type ALTERs
  with `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:1537` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1556` requires an InnoDB
  table rebuild when ALTER specifies `ROW_FORMAT` or `KEY_BLOCK_SIZE` because
  `.frm` and InnoDB dictionary attributes can otherwise disagree.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` reaches the copy-ALTER path at
  `mariadb/sql/sql_table.cc:11763`, creates the intermediate table around
  `mariadb/sql/sql_table.cc:11817`, copies rows through
  `copy_data_between_tables()` around `mariadb/sql/sql_table.cc:11943`, and
  swaps the rebuilt native files back to the final table name around
  `mariadb/sql/sql_table.cc:12135` through `mariadb/sql/sql_table.cc:12170`.
- `packages/libmylite/src/database.cc:ownerless_finish_dictionary_ddl()`
  already marks native file-operation checkpoint-needed evidence before the
  unsafe `dictionary-before-finish` hook can interrupt dictionary finish.
- `packages/libmylite/src/ownerless_dictionary_state.cc` stores an exact
  recoverable dictionary kind so a later process can finish only the same DDL
  class that the dead owner marked.

## Design

Add a new exact recovery kind for:

```sql
ALTER TABLE <schema>.<table> ROW_FORMAT=DYNAMIC
```

The classifier accepts only:

- `ALTER TABLE`;
- a qualified `schema.table` identifier;
- `ROW_FORMAT = DYNAMIC`;
- optional trailing semicolons.

It does not accept compressed row-format, `KEY_BLOCK_SIZE`, other row formats,
additional ALTER clauses, partition or tablespace options, multi-action ALTER,
or unqualified table names. That keeps live recovery tied to the single SQL
shape already covered by the focused crash selector.

The hook crash runner changes from a live-peer `MYLITE_BUSY` probe to a live
ownerless opener while the old peer remains held. The opener must:

- recover the dead writer's dictionary generation;
- observe `INNODB_SYS_TABLES.ROW_FORMAT = 'Dynamic'`;
- observe `information_schema.tables.row_format = 'Dynamic'`;
- read the two retained rows and payload lengths copied through the native
  rebuild;
- close while the native file-operation checkpoint marker remains set because
  another peer is still live.

After the held peer exits, the existing no-live ownerless opener verifies the
same rebuilt state, performs a post-recovery insert, closes, and requires the
native file-operation marker to be drained. The final state still has to survive
ownerless reopen, ordinary native exclusive reopen, forced `.shm` rebuild, and
ordinary native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- exact `ALTER TABLE schema.table ROW_FORMAT=DYNAMIC` live-peer dictionary
  recovery after a writer dies at `dictionary-before-finish`;
- retained dynamic row-format metadata, copied rows, copied payload bytes, and
  post-recovery writes;
- marker-retention proof while another live ownerless peer remains;
- final no-live marker-drain proof.

Out of scope:

- `ROW_FORMAT=COMPRESSED`, `KEY_BLOCK_SIZE`, redundant row-format, page
  compression, encryption, tablespace, external-directory, partition, and
  multi-action ALTER variants;
- charset, primary-key, foreign-key, generated-column, CHECK, and column
  rebuild variants;
- cross-schema or multi-table DDL recovery;
- SQL-level table-lock callback reachability;
- external MariaDB/RQG stress.

## Compatibility Impact

No new SQL syntax or public API surface is enabled. The slice strengthens the
ownerless concurrency guarantee for the already supported ordinary dynamic
row-format rebuild crash boundary by replacing no-live-only cleanup with
live-peer dictionary recovery for one exact statement shape.

Compressed row-format and other rebuild classes remain partial until their own
live-peer recovery evidence is added.

## DDL Metadata Routing Impact

The dictionary recovery protocol gains a distinct
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ROW_FORMAT_DYNAMIC` kind.
Dead-owner cleanup may finish an active dictionary generation with that kind
only when the dead owner marked the same kind. Mismatched recovery kinds remain
errors in primitive coverage.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable application state and
ownerless coordination files remain inside the MyLite database directory:

- `datadir/app/ownerless_row_format_base.frm`,
- `datadir/app/ownerless_row_format_base.ibd`,
- `concurrency/mylite-concurrency.ckpt`,
- `concurrency/mylite-concurrency.shm`.

Live recovery does not drain native file-operation checkpoint evidence while
another ownerless peer is live. The existing final no-live close path remains
responsible for native checkpoint proof and marker clearing.

## Native Storage Impact

MariaDB/InnoDB still owns the copy-style rebuild and native file swap. MyLite
does not rewrite table contents or row-format metadata. The slice only permits
the ownerless dictionary generation to be completed around the already-finished
native dynamic row-format rebuild.

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
  `dictionary-row-format-file-op-marker-crash`,
  `dictionary-row-format-crash`,
  `dictionary-force-rebuild-file-op-marker-crash`,
  `dictionary-compressed-row-format-file-op-marker-crash`, and
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`.
- Run the registered hook CTest file-op marker subset.
- Build production embedded targets:
  `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`.
- Run production primitive and adjacent ownerless SQL selectors, including
  `native-file-op-marker-drain`, `dictionary-row-format-file-op-marker-crash`,
  and `row-format-ddl`.
- Run `format-check`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Acceptance Criteria

- The focused row-format crash selectors reach the hook and do not hang.
- The killed row-format writer leaves durable native file-operation checkpoint
  evidence before live recovery.
- A new ownerless read/write opener succeeds while the original peer is still
  live.
- The live opener observes dynamic row-format metadata, retained rows, and
  retained payload bytes.
- The marker remains set after the live opener closes while another peer is
  still live.
- After the peer exits, no-live ownerless recovery supports a post-recovery
  insert and drains the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  observe the recovered dynamic row-format state.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- Hook selectors passed:
  `dictionary-row-format-file-op-marker-crash`, `dictionary-row-format-crash`,
  `dictionary-force-rebuild-file-op-marker-crash`,
  `dictionary-compressed-row-format-file-op-marker-crash`, and
  `dictionary-compressed-row-format-key-block-file-op-marker-crash`.
- Registered hook CTest subset passed 15/15, covering primitives plus the
  focused native file-operation marker crash family.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selectors passed: `native-file-op-marker-drain`,
  `dictionary-row-format-file-op-marker-crash`, `row-format-ddl`,
  `dictionary-force-rebuild-file-op-marker-crash`, and
  `force-rebuild-tablespace-replay`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed 1/1.
- `cmake --build --preset prod --target format-check` passed.
- `tools/check-ci-production-builds` passed.
- `git diff --check` passed.
- Cleanup scan found no `/tmp/mylite-ownerless-*` directories and no ownerless
  test processes.

## Risks And Unresolved Questions

- This exact classifier deliberately does not generalize to all row-format
  rebuilds or all copy-style ALTER statements.
- Compressed/key-block rebuilds have additional physical page-size and ZBLOB
  evidence requirements and remain separate live-recovery slices.
- Broader native redo/checkpoint reconciliation and external randomized stress
  remain ownerless completion gates.
