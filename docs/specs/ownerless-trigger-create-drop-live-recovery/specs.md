# Ownerless Trigger Create Drop Live Recovery

## Problem Statement

Ownerless trigger crash selectors already prove simple `CREATE TRIGGER` and
`DROP TRIGGER` native metadata survives a writer death after MariaDB updates
`.TRG`/`.TRN` files but before MyLite publishes ownerless dictionary finish.
Those selectors still require no-live recovery: while another ownerless peer
remains open, cleanup stays busy.

Simple trigger create/drop is metadata-only SQL-layer file metadata, not an
InnoDB file-per-table operation. MyLite should recover these focused trigger
boundaries while another ownerless peer remains live, using the same
metadata-only dictionary recovery lane used by focused view DDL.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_TRIGGER` and `SQLCOM_DROP_TRIGGER` dispatch to
    `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc`
  - `mysql_create_or_drop_trigger()` opens the subject table and routes
    accepted trigger DDL to `Table_triggers_list`.
  - `Table_triggers_list::create_trigger()` creates the trigger-name `.TRN`
    file and writes the table-level `.TRG` file.
  - `Table_triggers_list::drop_trigger()` removes the trigger from the list,
    removes or rewrites the table-level `.TRG` file, and removes the
    trigger-name `.TRN` file.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL begins before native SQL and the unsafe
    `dictionary-before-finish` hook runs after native SQL execution but before
    ownerless dictionary finish.
  - Existing metadata-only recovery covers focused view definitions without a
    native file-operation checkpoint marker.
- `packages/libmylite/src/ownerless_dictionary_state.h`
  - Recovery kinds are persisted as numeric dictionary-state marker classes and
    must be extended explicitly when a new DDL family becomes live-recoverable.

## Design

Add focused trigger create/drop recovery kinds:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER`

Classify only the already-covered simple trigger SQL shapes:

- `CREATE TRIGGER <trigger> BEFORE|AFTER INSERT|UPDATE|DELETE ON <table> FOR EACH ROW ...`
- `DROP TRIGGER <trigger>`

The classifier deliberately does not promote `CREATE OR REPLACE TRIGGER`,
`CREATE TRIGGER IF NOT EXISTS`, `DROP TRIGGER IF EXISTS`,
`DEFINER=CURRENT_USER`, or ordered triggers. MariaDB places
`FOLLOWS`/`PRECEDES` after `FOR EACH ROW`, so the classifier rejects those
ordering tokens before the body. Simple create trigger bodies, including the
covered delayed missing-dependency and stored-function-body selectors, share
the same syntax-level recovery kind.

Mark the new trigger recovery kinds metadata-only and include them in live-peer
dead-owner recovery. Update the existing simple trigger create/drop crash
selectors so they:

1. keep a live ownerless peer open after killing the writer,
2. open a new ownerless handle while that peer remains live,
3. recover the trigger create/drop boundary,
4. verify `.TRG`/`.TRN`, `INFORMATION_SCHEMA.TRIGGERS`,
   `SHOW CREATE TRIGGER`, and trigger firing/non-firing state,
5. verify the native file-operation checkpoint-needed marker remains clear,
6. release the held peer and keep existing ownerless/native reopen checks.

## Scope

In scope:

- Simple trigger create/drop dictionary prefinish crash boundaries.
- Live-peer metadata-only recovery for native `.TRG` and `.TRN` state.
- Delayed missing-dependency and stored-function-body simple `CREATE TRIGGER`
  selectors that share the syntax-level recovery kind.
- Trigger firing after recovered create and non-firing after recovered drop.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Primitive coverage for the new dictionary recovery kinds.

Out of scope:

- Trigger replacement, ordering, idempotent no-op, definer, privilege/security,
  and randomized trigger crash variants.
- Table/file-per-table DDL, schema DDL, foreign-key multi-DDL, and broader
  metadata-only DDL classes.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB trigger metadata changes can be finished
by a new ownerless opener while another ownerless peer remains live.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The slice continues to use native
MariaDB `.TRG` and `.TRN` files under `datadir/<schema>/`, ownerless dictionary
state under the MyLite concurrency directory, and the existing ownerless/native
reopen lifecycle.

## Native Storage Impact

Trigger metadata is SQL-layer native metadata. The base and audit tables are
InnoDB so tests also verify post-recovery DML durability, but the recovery
classification does not alter InnoDB redo/checkpoint policy, page-version WAL,
or file-per-table lifecycle handling.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Build and run focused ownerless primitive coverage for dictionary recovery
  kinds.
- Run focused hook selectors:
  - `dictionary-trigger-create-crash`
  - `dictionary-trigger-drop-crash`
- Run the covered simple-body create variants:
  - `dictionary-trigger-invalid-dependency-crash`
  - `dictionary-trigger-stored-function-crash`
- Run the registered trigger CTest coverage that remains separately reported.
- Run adjacent trigger crash selectors to confirm unpromoted variants still
  recover.
- Run production embedded trigger selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A killed simple trigger create/drop writer is recovered by a new ownerless
  opener while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for both
  trigger metadata-only recovery kinds.
- Recovered create exposes the trigger and fires it through base-table inserts.
- Recovered drop removes the trigger and keeps later base-table inserts from
  updating the audit table.
- Existing trigger replacement, ordering, idempotent, and definer crash
  selectors continue to pass as separate no-live recovery cases.
- Delayed missing-dependency and stored-function-body simple create selectors
  recover while a peer remains live, preserve their existing firing/error
  behavior, and keep the native file-operation marker clear at the recovery
  boundary.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
  passed.
- `ctest --preset ownerless-test-hooks -R
  '^(libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-trigger-definer-crash)$'
  --output-on-failure` passed: 2/2 in 5.57 seconds.
- Direct hook selectors passed:
  `dictionary-trigger-create-crash`, `dictionary-trigger-drop-crash`,
  `dictionary-trigger-replace-crash`, `dictionary-trigger-order-crash`,
  `dictionary-trigger-invalid-dependency-crash`,
  `dictionary-trigger-definer-crash`,
  `dictionary-trigger-stored-function-crash`,
  `dictionary-trigger-idempotent-create-crash`, and
  `dictionary-trigger-idempotent-drop-crash`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Production selectors passed: `trigger-ddl`, `trigger-ddl-variants`,
  `trigger-ordering`, `trigger-idempotent-ddl`, `routine-policy`, and
  `routine-execution-policy`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed: 1/1 in 77.32 seconds.
- `cmake --build --preset prod --target format-check` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed: 1/1 in 4.45 seconds.
- `git diff --check` passed.

## Risks And Follow-Up

- This covers only syntax-simple trigger create/drop live recovery.
- Trigger replacement, ordering, idempotent no-op, definer, privilege/security,
  and broader trigger metadata variants remain planned for live-recovery
  promotion.
- Broader schema metadata-only recovery and multi-table/cross-schema dropped
  tablespace live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
