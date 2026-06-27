# Ownerless Engine Rebuild Live Recovery

## Problem

Ownerless live recovery already covers representative ALTER rebuild spellings:
`ALTER TABLE ... FORCE`, `ROW_FORMAT=DYNAMIC`, compressed row-format key-block
variants, and charset conversion. MariaDB also treats
`ALTER TABLE ... ENGINE=InnoDB` against an existing InnoDB table as a common
same-engine rebuild request. That spelling still sat in the broader ALTER
rebuild gap, leaving one common native file-lifecycle boundary without focused
live-peer crash evidence.

## Source Findings

- Base: MariaDB 11.8.6, `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy` parses table options, including `ENGINE`, through
  `create_table_options_space_separated` in `ALTER TABLE` and marks
  `HA_CREATE_USED_ENGINE`.
- `mariadb/sql/sql_table.cc` records whether the ALTER used an explicit engine,
  resolves the target engine, and when the target engine equals the original
  engine, sets `ALTER_RECREATE` so `ALTER TABLE ... ENGINE` can request a table
  rebuild.
- `mariadb/storage/innobase/handler/handler0alter.cc` treats rebuild-needed
  ALTER options through the InnoDB alter/rebuild path.

## Design

- Classify only the bounded same-engine spelling:
  `ALTER TABLE [schema.]table ENGINE [=] InnoDB`.
- Before assigning a recoverable dictionary kind, verify the source table is a
  real `InnoDB` base table in `information_schema.tables`.
- Reuse the existing `ALTER_TABLE_FORCE_REBUILD` dictionary recovery kind
  because this spelling has the same ownerless recovery policy: physical native
  rebuild work with the native file-operation marker retained until final
  no-live drain.
- Do not broaden this slice to engine switches, partitioned tables, mixed ALTER
  clauses, temporary tables, or non-InnoDB engines. The exact
  `ALGORITHM=COPY, LOCK=EXCLUSIVE` tail is covered separately by
  `docs/specs/ownerless-engine-rebuild-copy-lock-live-recovery/specs.md`;
  broader algorithm/lock option matrices remain out of scope.

## Compatibility Impact

Successful SQL behavior is unchanged and remains MariaDB-compatible. The slice
only changes ownerless crash recovery after MariaDB has successfully completed a
bounded same-engine InnoDB rebuild and the writer dies before ownerless
dictionary finish.

## Storage And Lifecycle Impact

Durable state remains in the MyLite database directory. The completed native
rebuild retains the existing native file-operation checkpoint marker while a
peer remains live, then drains that marker during final no-live recovery.

## Tests

- Add a hook-build selector that creates an InnoDB file-per-table table with a
  secondary index and payload rows, executes `ALTER TABLE ... ENGINE=InnoDB`
  under the `dictionary-before-finish` hook, kills the writer, recovers while a
  peer remains live, verifies marker retention, and checks final ownerless and
  native reopen before and after forced `.shm` rebuild.
- Add a production-safe normal selector, `engine-rebuild-ddl`, that exercises
  already-open peer refresh for the same SQL spelling and final ownerless/native
  reopen checks.
- Register the selector as a standalone hook CTest for visible CI timing.
- Run adjacent force/row-format/charset rebuild crash selectors, ownerless DDL
  stress, production focused rebuild selectors, format, and diff checks.

## Acceptance Criteria

- A killed same-engine `ALTER TABLE ... ENGINE=InnoDB` writer can be recovered
  while another ownerless peer remains open.
- The rebuilt table, secondary index, and rows remain usable during live-peer
  recovery.
- The native file-operation marker remains set while a peer is live and drains
  after final no-live recovery.
- Ownerless/native reopen and forced shared-memory rebuild observe the same
  final table state.
