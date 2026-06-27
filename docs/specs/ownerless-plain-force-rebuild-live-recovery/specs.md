# Ownerless Plain Force Rebuild Live Recovery

## Problem Statement

The force-rebuild live-recovery slice covered the explicit copy-algorithm
shape:

```sql
ALTER TABLE schema.table FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE
```

MariaDB also accepts the common plain form:

```sql
ALTER TABLE schema.table FORCE
```

That plain statement still enters MariaDB's rebuild path, can emit native
file-operation redo, and can be killed after native completion but before
MyLite publishes ownerless dictionary finish. Leaving it unclassified keeps a
real `ALTER TABLE ... FORCE` compatibility shape on the no-live-only path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:8155-8158` parses `ALTER TABLE ... FORCE` by
  setting `ALTER_RECREATE`.
- `mariadb/sql/handler.h:707-708` documents `ALTER_RECREATE` for `FORCE`,
  same-engine `ENGINE`, and recreate-table operations.
- `mariadb/sql/sql_table.cc:11402-11420` treats standalone `FORCE` as an
  identical-table recreate request.
- `mariadb/storage/innobase/handler/handler0alter.cc:7237-7245` handles
  InnoDB `ALTER TABLE ... FORCE` as a rebuild class.
- `packages/libmylite/src/database.cc` already has a file-op recovery kind for
  the explicit force-rebuild shape and keeps the native file-operation marker
  durable until final no-live drain.

## Scope And Non-Goals

In scope:

- Accept schema-qualified `ALTER TABLE schema.table FORCE` as the same
  recoverable force-rebuild kind used by the explicit copy/lock variant.
- Preserve the existing explicit `FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`
  classifier behavior.
- Add a focused unsafe hook CTest selector that kills a plain-force writer at
  `dictionary-before-finish` while a live peer remains open.
- Verify rebuilt table metadata, secondary-index use, copied payloads, marker
  retention while live, marker drain after no-live recovery, ownerless/native
  reopen, and forced `.shm` rebuild.

Out of scope:

- Unqualified table names.
- Other algorithm/lock orderings or lock modes.
- OPTIMIZE TABLE, partition rebuilds, import/discard tablespace, special-index
  rebuilds, and external-directory tables.
- External randomized MariaDB/RQG stress.

## Design

Extend `ownerless_alter_table_force_rebuild_recovery_statement()` from one exact
shape to two exact schema-qualified shapes:

- `ALTER TABLE schema.table FORCE`;
- `ALTER TABLE schema.table FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`.

Both return `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD`.
That recovery kind already forces the native file-operation checkpoint-needed
marker and participates in dead-owner live dictionary cleanup. No durable format
or enum change is needed.

## Compatibility Impact

No SQL grammar is added beyond MariaDB. MyLite's ownerless crash boundary now
matches the plain `ALTER TABLE ... FORCE` form that applications may issue, not
only the explicit copy/lock spelling used by the original hook fixture.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format change. MariaDB/InnoDB remains the storage
authority for the rebuilt `.frm`/`.ibd` state. MyLite only records the completed
statement as recoverable ownerless dictionary state and keeps the existing
native file-operation checkpoint marker durable until no live peers remain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds a small parser branch, one hook selector, CTest
registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `libmylite.ownerless-dictionary-force-rebuild-plain-crash`.
- Run adjacent force/engine rebuild marker selectors.
- Run production `mylite` build, format-check, CI production-build guard, and
  `git diff --check`.

## Acceptance Criteria

- Plain schema-qualified `ALTER TABLE ... FORCE` is marked recoverable before
  dictionary finish.
- The existing explicit copy/lock force-rebuild selector still passes.
- The plain-force hook selector recovers while a peer remains live, observes the
  native file-operation marker set until the peer releases, drains it after
  no-live recovery, and preserves rebuilt table/index/payload state through
  ownerless/native reopen and forced `.shm` rebuild.

## Risks And Follow-Up

- This is still a token-level classifier, not a MariaDB parser replacement.
- Broader rebuild spellings and partition/import/special-index classes remain
  planned separately because their native file lifecycle is wider than plain
  force rebuild.
