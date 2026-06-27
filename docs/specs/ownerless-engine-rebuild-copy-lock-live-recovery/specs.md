# Ownerless Engine Rebuild Copy Lock Live Recovery

## Problem Statement

Ownerless live recovery covered the common same-engine rebuild spelling:

```sql
ALTER TABLE schema.table ENGINE=InnoDB
```

MariaDB also accepts explicit ALTER options that applications use when they
want table-copy semantics and an exclusive lock:

```sql
ALTER TABLE schema.table ENGINE=InnoDB, ALGORITHM=COPY, LOCK=EXCLUSIVE
```

Before this slice, MyLite's ownerless dictionary classifier recognized the
plain same-engine rebuild but left this explicit copy/lock spelling on the
broader no-live recovery path. That left a common native rebuild boundary
without focused live-peer crash evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5693-5708` parses `ENGINE [=] ident_or_text` as a
  table option and sets `HA_CREATE_USED_ENGINE`.
- `mariadb/sql/sql_yacc.yy:8150-8165` allows table options, `FORCE`,
  `alter_algorithm_option`, and `alter_lock_option` as ALTER list elements, so
  the explicit engine/copy/lock order is a normal MariaDB ALTER spelling.
- `mariadb/sql/handler.h:707-708` documents `ALTER_RECREATE` for `FORCE`,
  same-engine `ENGINE`, and recreate-table operations.
- `mariadb/sql/sql_table.cc:10834-10839` treats a requested lock stronger than
  `LOCK=NONE` and algorithms beyond copy as non-online ALTER work.
- `mariadb/sql/sql_table.cc:11405-11423` treats same-engine
  `ALTER TABLE ... ENGINE` as a table rebuild by setting `ALTER_RECREATE`.
- `mariadb/storage/innobase/handler/handler0alter.cc:7237-7245` handles
  rebuild-only InnoDB ALTER work through the recreate-table class.

## Scope And Non-Goals

In scope:

- Accept `ALTER TABLE [schema.]table ENGINE=InnoDB, ALGORITHM=COPY,
  LOCK=EXCLUSIVE` as the same recoverable ownerless dictionary kind used by
  the plain same-engine rebuild.
- Preserve the existing pre-execution metadata check that the source table is a
  real InnoDB base table.
- Add focused hook coverage that kills the writer at `dictionary-before-finish`
  while another ownerless peer remains live.
- Verify rebuilt rows, secondary-index metadata and use, native file-operation
  marker retention while live, final no-live drain, ownerless/native reopen,
  and forced `.shm` rebuild.

Out of scope:

- Engine switches, non-InnoDB engines, partitioned tables, temporary tables,
  import/discard tablespace, OPTIMIZE TABLE, or special index rebuilds.
- Other `ALGORITHM`/`LOCK` values, omitted commas, or option order matrices.
- External randomized MariaDB/RQG stress.

## Design

Extend the same-engine InnoDB rebuild classifier from one exact tail shape to
two:

- end of statement after `ENGINE [=] InnoDB`;
- `, ALGORITHM=COPY, LOCK=EXCLUSIVE` followed only by optional semicolons.

The explicit copy/lock spelling returns
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD`, matching the
plain same-engine rebuild. That recovery kind already owns live-peer
dictionary cleanup and keeps the native file-operation checkpoint marker
durable until the final no-live drain.

To avoid parser drift between force and same-engine rebuild handling, the exact
copy/exclusive tail is parsed through one small helper reused by both
classifiers.

## Compatibility Impact

SQL semantics remain MariaDB-owned. MyLite only broadens ownerless crash
classification after MariaDB has successfully completed the bounded same-engine
rebuild and the writer dies before ownerless dictionary finish.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. MariaDB/InnoDB remains the
authority for the rebuilt `.frm` and `.ibd` files in the MyLite database
directory. MyLite persists the completed DDL generation as recoverable
ownerless dictionary state and leaves native file-operation marker draining to
the existing no-live checkpoint path.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds a bounded classifier tail, one unsafe hook selector,
CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `dictionary-engine-rebuild-copy-lock-crash` directly and through CTest.
- Run adjacent force and same-engine rebuild crash selectors.
- Run production `mylite` build, format, CI production-build guard, and
  `git diff --check`.

## Acceptance Criteria

- Explicit same-engine `ENGINE=InnoDB, ALGORITHM=COPY, LOCK=EXCLUSIVE` is
  marked recoverable only after source metadata proves the table is InnoDB.
- A killed writer at `dictionary-before-finish` recovers while a peer remains
  live.
- Rebuilt rows and secondary-index behavior survive live recovery, final
  no-live drain, ownerless/native reopen, and forced `.shm` rebuild.
- The native file-operation marker remains durable while the peer is live and
  drains after final no-live recovery.

## Risks And Follow-Up

- This remains a token-level classifier for an exact MariaDB-compatible
  spelling, not a general ALTER parser.
- Broader rebuild classes and option matrices remain planned separately because
  their native file lifecycle can differ from the bounded same-engine rebuild.
