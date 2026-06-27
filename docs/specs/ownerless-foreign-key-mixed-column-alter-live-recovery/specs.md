# Ownerless Foreign-Key Mixed Column ALTER Live Recovery

## Problem Statement

Ownerless live recovery covers focused `ALTER TABLE ... ADD CONSTRAINT ...
FOREIGN KEY`, `DROP FOREIGN KEY`, pure comma-separated foreign-key add/drop
lists, and FK-only mixed drop/add lists. The remaining DDL matrix still calls
out mixed comma-separated non-rename foreign-key ALTER statements. A practical
high-risk case combines native foreign-key metadata removal, ordinary table
definition mutation, and foreign-key metadata creation in one MariaDB ALTER:

```sql
ALTER TABLE schema.child
  DROP FOREIGN KEY old_fk,
  ADD COLUMN note INT NOT NULL DEFAULT 7,
  ADD CONSTRAINT new_fk FOREIGN KEY (parent_id) REFERENCES schema.parent (id)
```

Before this slice, the mixed-FK ownerless recovery classifier accepted only
FK ADD and FK DROP clauses. Adding a column in the same ALTER list left the
statement outside focused live-peer recovery even though each component had
bounded evidence in isolation.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:6153-6167` parses table-level
  `FOREIGN KEY` clauses and records them in the ALTER key list.
- `mariadb/sql/sql_yacc.yy:8051-8059` parses
  `ALTER TABLE ... DROP FOREIGN KEY`.
- `mariadb/sql/sql_yacc.yy:8150-8165` admits comma-separated ALTER table
  options and elements in one ALTER list.
- `mariadb/sql/sql_table.cc:7313-7328` maps newly added non-generated fields
  to stored base-column ALTER flags.
- `mariadb/storage/innobase/handler/handler0alter.cc:123-126` classifies
  foreign-key ADD/DROP as InnoDB foreign-key schema operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:1895-1943` treats stored
  base-column ADD as instant/rebuild-avoiding only for constrained flag sets.
- `mariadb/storage/innobase/handler/handler0alter.cc:10046-10130` applies
  added/dropped foreign-key constraints to persistent InnoDB dictionary tables.
- `mariadb/storage/innobase/handler/handler0alter.cc:11701-11723` commits the
  native ALTER transaction before post-commit cache refresh.

## Scope And Non-Goals

In scope:

- A focused schema-qualified ALTER list with one `DROP FOREIGN KEY`, one
  simple `ADD COLUMN note INT NOT NULL DEFAULT 7`, and one
  `ADD CONSTRAINT ... FOREIGN KEY ... REFERENCES ...` clause.
- Live-peer dictionary recovery after a writer is killed at
  `dictionary-before-finish`.
- Native file-operation marker retention while a peer remains live and final
  no-live drain after the peer exits.
- Recovered FK metadata, FK enforcement, added-column metadata/defaults,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Arbitrary ALTER list parsing.
- Multiple non-FK clauses, column placement, generated columns,
  `AUTO_INCREMENT`, index/primary-key/check clauses, storage options, explicit
  `ALGORITHM`/`LOCK` matrices, and partition/table-admin paths.
- SQL-level table-lock callback reachability.
- Randomized external MariaDB/RQG stress.

## Design

Add a clause-level ownerless ADD COLUMN recognizer that reuses the same
conservative grammar as the focused single-clause ADD COLUMN live-recovery
classifier but stops at a comma or statement terminator. The recognizer accepts
only a real ADD COLUMN branch when pre-execution metadata proves the column is
absent, and it keeps rejecting keys, constraints, generated columns, placement,
storage, and algorithm/lock clauses.

Extend the mixed-FK classifier to accept:

- existing FK ADD clauses,
- existing FK DROP clauses,
- the new simple ADD COLUMN clause.

The mixed-FK classifier reports whether it saw ADD COLUMN. FK-only mixed
ALTERs keep using the metadata-only ADD FOREIGN KEY recovery kind. Mixed
FK-plus-column ALTERs use the existing ADD COLUMN recovery kind so the native
file-operation checkpoint marker is written before dictionary finish and
retained until no-live checkpoint drain.

## Compatibility Impact

No SQL feature is newly enabled. MariaDB remains the syntax and native storage
authority. MyLite broadens only the ownerless crash classification for a
completed MariaDB ALTER that has already succeeded natively.

## Directory, Lifecycle, And Native Storage Impact

No durable format or directory-layout change. The slice exercises the existing
ownerless dictionary recovery record, native file-operation checkpoint marker,
live-peer cleanup, no-live drain, ownerless/native reopen, and forced shared
memory rebuild paths.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds bounded classifier logic, one unsafe hook selector,
CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `dictionary-foreign-key-mixed-column-alter-crash` directly and through
  CTest.
- Run adjacent FK ADD/DROP/mixed crash selectors.
- Run production `mylite` build, CI production-build guard, format check, PHP
  embedded production ownerless test binary build, and `git diff --check`.

## Acceptance Criteria

- The new selector reaches `dictionary-before-finish` and kills the writer
  after native ALTER success.
- A later ownerless opener recovers while the original peer remains live.
- The native file-operation checkpoint marker stays set while the peer remains
  live and clears after final no-live recovery.
- Recovered metadata shows the old FK absent, the retained FK present, the new
  FK present, and `note INT NOT NULL DEFAULT 7` present.
- Existing and later rows observe the default value, invalid parent references
  fail, and a formerly FK-protected old-parent column can accept orphan values.
- Ownerless and native reopen before and after forced `.shm` rebuild observe
  the same table definition, rows, and FK enforcement.

## Risks And Follow-Up

- This is a focused exact-shape classifier, not a full MariaDB ALTER parser.
- Broader FK plus non-FK ALTER lists, other column mutations, index/check
  combinations, schema option variants, and external randomized oracle stress
  remain ownerless completion work.
