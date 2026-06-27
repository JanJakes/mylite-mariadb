# Ownerless Foreign-Key Live Recovery

## Problem Statement

Ownerless foreign-key crash coverage already killed completed
`ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` and
`ALTER TABLE ... DROP FOREIGN KEY` writers at the
`dictionary-before-finish` hook, then proved the completed native InnoDB
metadata change after the live peer exited. That left the live-recovery claim
weaker than adjacent metadata-only DDL classes: while another ownerless process
remained open, cleanup still had to stay busy until no-live recovery.

This slice promotes bounded ordinary non-rename foreign-key DDL to the
recoverable metadata-only live-peer lane. Generated-column child or referenced
tables stay on the conservative native-file recovery lane because hook
coverage showed those ALTER forms can change native table identity.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:6153` through
  `mariadb/sql/sql_yacc.yy:6167` parses table-level
  `FOREIGN KEY` definitions and records them in the ALTER key list.
- `mariadb/sql/sql_yacc.yy:8051` through
  `mariadb/sql/sql_yacc.yy:8059` parses `DROP FOREIGN KEY` and records
  `ALTER_DROP_FOREIGN_KEY`.
- `mariadb/storage/innobase/handler/handler0alter.cc:123` through
  `mariadb/storage/innobase/handler/handler0alter.cc:126` classifies
  add/drop foreign-key operations as schema-only foreign-key operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:2295` through
  `mariadb/storage/innobase/handler/handler0alter.cc:2301` documents the
  InnoDB online ADD FOREIGN KEY condition.
- `mariadb/storage/innobase/handler/handler0alter.cc:3219` through
  `mariadb/storage/innobase/handler/handler0alter.cc:3413` builds added
  foreign-key descriptors and validates child/parent index metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc:11701` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11723` commits the
  native InnoDB ALTER transaction before post-commit cache refresh.
- `mariadb/storage/innobase/handler/handler0alter.cc:11743` through
  `mariadb/storage/innobase/handler/handler0alter.cc:11767` refreshes
  InnoDB foreign-key cache state after the native ALTER commit.

## Design

Add two MyLite dictionary-recovery marker kinds for completed FK ALTER
boundaries:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ADD_FOREIGN_KEY`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_DROP_FOREIGN_KEY`

The classifier accepts bounded FK-only statements when neither the child table
nor the referenced table has generated columns:

- `ALTER TABLE <table> ADD [CONSTRAINT <name>] FOREIGN KEY (...) REFERENCES <table> (...)`
  with optional FK action tail,
- pure comma-separated lists of the same FK ADD clause form,
- `ALTER TABLE <table> DROP FOREIGN KEY <name>`,
- pure comma-separated lists of the same FK DROP clause form.

These are treated as metadata-only ownerless recovery kinds because the covered
boundary changes SQL/InnoDB dictionary metadata rather than creating, deleting,
renaming, truncating, or rebuilding native table files. The existing native
file-operation marker remains clear, and file-operation DDL remains on the
marker-retaining recovery lane.

InnoDB can still publish a file-operation redo observation while committing
foreign-key metadata. The ownerless dictionary finish path consumes that
observation for these two recovery kinds without writing the MyLite
native-file checkpoint-needed marker; the recoverable dictionary marker is the
durable proof used by live-peer cleanup.

The ADD classifier resolves the referenced table from the statement text. The
DROP classifier resolves it from `information_schema.referential_constraints`
before deciding whether the operation is eligible for metadata-only live
recovery. If either lookup or the generated-column probe is uncertain, MyLite
keeps the existing conservative recovery behavior.

## Scope And Non-Goals

In scope:

- ordinary FK ADD and DROP prefinish crash recovery while a peer remains live,
- pure comma-separated ordinary FK-only ADD and DROP list prefinish crash
  recovery while a peer remains live,
- marker-clear assertions for each covered ordinary live-recovery boundary,
- direct CTest registrations for the ordinary focused FK crash selectors,
- regression coverage proving generated-column FK ADD and DROP remain
  conservative and recover only after the live peer exits.

Out of scope:

- mixed comma-separated multi-clause ALTER statements that combine FK
  operations with non-FK operations,
- metadata-only live recovery for generated-column child or referenced FK
  ALTER forms,
- FK rename recovery, already covered by rename-list live recovery,
- referential-action DML crash fuzzing beyond existing deterministic row
  boundaries,
- full external MariaDB/RQG FK graph execution.

## Compatibility Impact

No SQL feature is newly enabled. Supported ownerless FK DDL keeps the same
MariaDB syntax and behavior, but an ordinary FK-only writer death after native
FK metadata commit and before MyLite dictionary finish can now be recovered by
another live ownerless opener instead of waiting for no-live recovery. This
follow-up extends the same live-recovery lane to pure comma-separated ordinary
FK ADD and DROP lists while leaving mixed FK/non-FK ALTER lists conservative.
Generated-column FK DDL remains supported, but its crash boundary retains the
native-file marker and waits for no-live recovery.

## Database Directory And Lifecycle Impact

No directory layout changes are introduced. Durable metadata remains in the
MyLite database directory through MariaDB/InnoDB native dictionary storage.
The slice changes only ownerless shared dictionary-generation recovery around a
completed native boundary.

## Native Storage Impact

MyLite does not reinterpret foreign-key metadata. It records that the completed
native FK ALTER reached the post-MariaDB-success prefinish window, lets a live
peer publish the ownerless dictionary generation, suppresses the MyLite
native-file checkpoint marker for the covered FK-only ALTER forms, and verifies
FK metadata, enforcement, ownerless reopen, native exclusive reopen, and forced
`.shm` rebuild.

## Public API, Build, Size, And Dependencies

No public API, build profile, binary-size, license, or dependency changes.

## Test And Verification Plan

- Build `mylite_ownerless_primitives_test` and
  `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run `libmylite.ownerless-primitives`.
- Run the four focused hook selectors:
  `dictionary-foreign-key-crash`,
  `dictionary-foreign-key-drop-crash`,
  `dictionary-generated-column-foreign-key-crash`, and
  `dictionary-generated-column-foreign-key-drop-crash`.
- Run the matching standalone CTests once registered.
- Run focused production ownerless FK selectors, ownerless stress as needed,
  `format-check`, `tools.check-ci-production-builds`, and `git diff --check`.

## Acceptance Criteria

- Each covered FK crash selector kills the writer at
  `dictionary-before-finish`, opens a new ownerless connection while another
  peer remains live, and observes the completed FK metadata change.
- The native file-operation checkpoint-needed marker remains clear before and
  after ordinary FK live recovery.
- Ordinary FK ADD recovery preserves FK metadata and enforcement.
- Ordinary FK DROP recovery preserves FK metadata absence and permits
  post-drop orphan/parent writes.
- Generated-column FK ADD and DROP crash selectors keep live-peer recovery
  busy, recover after the peer exits, and preserve generated-column FK state.
- Ownerless reopen, native exclusive reopen, and forced `.shm` rebuild all
  preserve the recovered state.

## Risks And Unresolved Questions

- Multi-clause ALTER statements that combine FK operations with other metadata
  or file-changing operations remain outside this slice.
- Generated-column FK metadata-only recovery is deliberately not claimed until
  native table identity/file lifecycle effects are classified more precisely.
- This is deterministic hook coverage, not long-running randomized FK/RQG
  stress.
