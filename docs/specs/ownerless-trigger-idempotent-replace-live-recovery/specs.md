# Ownerless Trigger Idempotent Replace Live Recovery

## Problem Statement

Ownerless trigger create/drop live recovery already covers syntax-simple
metadata-only trigger DDL. Trigger replacement and idempotent trigger no-op
crash selectors also exist, but they still prove only no-live recovery: a live
ownerless peer keeps cleanup busy until the peer exits.

`CREATE OR REPLACE TRIGGER`, `CREATE TRIGGER IF NOT EXISTS`, and
`DROP TRIGGER IF EXISTS` are still native MariaDB trigger metadata operations
over `.TRG` and `.TRN` files. MyLite should recover these bounded metadata-only
dictionary boundaries while another ownerless peer remains live, without
claiming broader trigger ordering, definer/security, or privilege semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses trigger DDL through
  `create_or_replace definer_opt TRIGGER_SYM ... trigger_tail`, with
  `opt_if_not_exists` inside `trigger_tail`. `DROP TRIGGER` parses
  `opt_if_exists` before the trigger name.
- `mariadb/sql/sql_trigger.cc` implements `Table_triggers_list::create_trigger()`.
  When the trigger-name `.TRN` file already exists, `OR REPLACE` backs up and
  drops the old trigger before writing the replacement, while `IF NOT EXISTS`
  returns success with a note and preserves the existing trigger definition.
- `mariadb/sql/sql_trigger.cc` implements missing `DROP TRIGGER IF EXISTS` by
  returning success with a note before table trigger files are mutated.
- Existing MyLite trigger live recovery already treats
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER` and
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TRIGGER` as metadata-only recovery
  kinds.
- Existing hook selectors cover trigger replacement and trigger idempotent
  create/drop crash boundaries, but currently use the no-live recovery helper.

## Design

- Broaden `ownerless_create_trigger_recovery_statement()` to accept:
  - `CREATE OR REPLACE TRIGGER <trigger> ...`
  - `CREATE TRIGGER IF NOT EXISTS <trigger> ...`
- Keep the existing ordered-trigger guard: if the token after `FOR EACH ROW` is
  `FOLLOWS` or `PRECEDES`, the statement is not promoted to live recovery.
- Do not accept `DEFINER` forms in this slice. Explicit-definer trigger crash
  coverage remains no-live until its security/privilege semantics are promoted
  separately.
- Broaden `ownerless_drop_trigger_recovery_statement()` to accept
  `DROP TRIGGER IF EXISTS <trigger>`.
- Reuse the existing create/drop trigger recovery kinds. No new dictionary-state
  marker numbers are required.
- Promote the existing replacement and idempotent create/drop hook selectors to
  use a held-live-peer recovery path, and assert that the native file-operation
  checkpoint-needed marker remains clear before and after live recovery.
- Register the promoted selectors as standalone hook CTests for visible timing
  and failure attribution.

## Scope

In scope:

- `CREATE OR REPLACE TRIGGER` replacement metadata live recovery.
- Duplicate `CREATE TRIGGER IF NOT EXISTS` no-op live recovery.
- Missing `DROP TRIGGER IF EXISTS` no-op live recovery.
- `.TRG`/`.TRN`, `INFORMATION_SCHEMA.TRIGGERS`, `SHOW CREATE TRIGGER`, trigger
  body preservation or replacement, trigger firing, ownerless/native reopen, and
  forced `.shm` rebuild checks.

Out of scope:

- `CREATE TRIGGER ... FOLLOWS|PRECEDES ...` ordering live recovery.
- `CREATE DEFINER=... TRIGGER` live recovery.
- Trigger privilege/security and randomized trigger crash matrices.
- Stored routine DDL/execution support.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless recovery
compatibility by allowing a completed MariaDB trigger replacement or idempotent
trigger no-op to finish ownerless dictionary recovery while another ownerless
peer remains live.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The slice continues to use MariaDB
native `.TRG` and `.TRN` metadata files under `datadir/<schema>/`, the existing
ownerless dictionary generation state, and the existing ownerless/native reopen
lifecycle.

## Native Storage Impact

Trigger metadata is SQL-layer native metadata. The tests use InnoDB base/audit
tables to verify post-recovery behavior, but the recovery classification does
not alter InnoDB redo/checkpoint policy, page-version WAL, or file-per-table
lifecycles.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-trigger-replace-crash`
  - `dictionary-trigger-idempotent-create-crash`
  - `dictionary-trigger-idempotent-drop-crash`
- Run adjacent hook trigger selectors, including create/drop, ordering,
  invalid-dependency, definer, and stored-function trigger crash coverage.
- Run production embedded trigger selectors:
  - `trigger-ddl`
  - `trigger-ddl-variants`
  - `trigger-ordering`
  - `trigger-idempotent-ddl`
- Run ownerless DDL stress because dictionary recovery classification changed.
- Run `format-check`, CI production-build guards, and `git diff --check`.

## Acceptance Criteria

- Replacement trigger recovery completes while another ownerless peer remains
  live, keeps the native file-operation marker clear, and fires the replacement
  body after recovery.
- Duplicate idempotent create recovery completes while another ownerless peer
  remains live, keeps the native file-operation marker clear, and preserves the
  original trigger body.
- Missing idempotent drop recovery completes while another ownerless peer
  remains live, keeps the native file-operation marker clear, preserves the real
  trigger, and keeps the missing `.TRN` absent.
- Ordered-trigger and explicit-definer crash selector live recovery is covered
  by `docs/specs/ownerless-trigger-order-definer-live-recovery/specs.md`.
