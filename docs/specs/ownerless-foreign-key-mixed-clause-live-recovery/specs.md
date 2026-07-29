# Ownerless Foreign-Key Mixed-Clause Live Recovery

## Problem Statement

Ownerless live-peer dictionary recovery now covers single foreign-key ADD/DROP
ALTERs and pure comma-separated foreign-key ADD or DROP lists. A successful
native `ALTER TABLE` that mixes only FK DROP and FK ADD clauses can still be
killed after MariaDB finishes native dictionary work and before MyLite publishes
dictionary finish. Without a recoverable metadata-only ownerless marker, a live
peer cannot prove that the dropped and added FK metadata are the durable state.

The high-risk bounded gap is FK-only mixed ALTER, for example:

```sql
ALTER TABLE child
  DROP FOREIGN KEY fk_old,
  ADD CONSTRAINT fk_new FOREIGN KEY (new_parent_id) REFERENCES parent_new(id)
```

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:6153-6165` parses table-level `FOREIGN KEY`
  definitions into ALTER key metadata.
- `mariadb/sql/sql_yacc.yy:8051` parses `DROP FOREIGN KEY` table elements into
  the ALTER drop list.
- `mariadb/sql/sql_table.cc:6578-6675` handles ADD FOREIGN KEY duplicate and
  metadata validation paths.
- `mariadb/sql/sql_table.cc:9586-9592` validates DROP FOREIGN KEY names against
  the child table's existing FK metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc:123-126` classifies
  InnoDB FK add/drop as schema-only alter operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:10046-10133` updates
  InnoDB FK dictionary definitions during ALTER completion.
- `packages/libmylite/src/database.cc` treats the existing FK ADD and FK DROP
  recovery kinds as metadata-only. The release-stabilization follow-up prearms
  native dictionary checkpoint evidence before those statements.

## Scope And Non-Goals

In scope:

- Accept comma-separated mixed FK-only ALTER lists containing at least one
  `DROP FOREIGN KEY` clause and at least one `ADD [CONSTRAINT] FOREIGN KEY ...
  REFERENCES ...` clause.
- Preserve the existing ordinary-table generated-column guard for the child and
  referenced tables.
- Reuse the existing metadata-only FK recovery lane because the recovery kind
  controls MyLite live-peer cleanup and native dictionary marker policy.
- Add a focused unsafe hook CTest selector that crashes at
  `dictionary-before-finish` while a live ownerless peer remains open.

Out of scope:

- Mixed ALTER lists that combine FK clauses with column, index, CHECK, rebuild,
  table-option, rename, or file-moving operations.
- Generated-column FK live recovery.
- SQL-level table-lock fault injection.
- External MariaDB/RQG stress expansion.

## Design

The FK statement classifier gains a third recognizer after the pure ADD and
pure DROP recognizers:

- parse the `ALTER TABLE` target once;
- prove the child table has no generated columns;
- consume one or more comma-separated clauses;
- accept only existing FK ADD or FK DROP clause shapes;
- require both an ADD clause and a DROP clause;
- for ADD clauses, prove each referenced table has no generated columns;
- for DROP clauses, resolve the existing FK's referenced table before
  execution and apply the same generated-column guard.

The classifier returns
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ADD_FOREIGN_KEY` for accepted
mixed FK-only statements. That kind is metadata-only but prearms native
dictionary checkpoint evidence until a successful ownerless dictionary finish
or recovery-capable no-live drain. No durable enum, shared-state format, or SQL
rewrite changes are needed.

## Compatibility Impact

No SQL grammar is added beyond MariaDB. The change lets MyLite's ownerless
recovery protocol cover a MariaDB-compatible FK-only multi-clause ALTER shape
that applications may issue as one statement. Broader mixed ALTERs remain
conservative and must be classified separately before MyLite claims recovery.

## Directory, Lifecycle, And Native Storage Impact

No directory layout, native storage format, or public lifecycle contract change.
InnoDB remains responsible for native FK dictionary updates. MyLite only records
that the finished statement is a metadata-only dictionary recovery boundary
while the crashing writer's ownerless state is recovered by a live peer.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The slice
adds a small classifier helper, one focused hook test, a CTest registration, and
documentation.

## Test And Verification Plan

- Add selector `dictionary-foreign-key-mixed-alter-crash`.
- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the new selector directly through CTest.
- Run adjacent FK dictionary crash selectors:
  single ADD, single DROP, pure multi-ADD, pure multi-DROP, and mixed ADD/DROP.
- Verify the mixed test observes:
  - dropped FK metadata absent;
  - retained and added FK metadata present;
  - dropped-parent violations allowed;
  - retained and added parent violations rejected;
  - native dictionary checkpoint marker retained until recovery-capable
    no-live drain;
  - ownerless reopen, native reopen, and forced `.shm` rebuild preserve state.
- Run production format/build guards and `git diff --check`.

## Acceptance Criteria

- FK-only mixed ADD/DROP ALTER statements are marked recoverable before
  dictionary finish.
- The native file-operation checkpoint marker remains set for this crashed
  metadata-only recovery class because committed InnoDB dictionary redo still
  needs startup authority.
- The focused hook CTest passes and proves recovered metadata and enforcement
  while a peer is live and after ownerless/native reopen.
- Existing pure FK ADD/DROP recognizers and generated-column rejection behavior
  remain unchanged.
- Mixed FK plus non-FK ALTERs remain unclaimed.

## Risks And Follow-Up

- This remains a token-level recognizer and deliberately does not replace
  MariaDB parsing.
- Broader mixed ALTER lists that include file-moving or rebuild work still need
  separate native checkpoint/file-lifecycle classification.
- Generated-column FK live recovery remains conservative and planned.
