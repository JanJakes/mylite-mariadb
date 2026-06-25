# Ownerless View Column-List Live Recovery

## Problem Statement

Ownerless view metadata live recovery covers simple `CREATE VIEW`, `DROP VIEW`,
`CREATE OR REPLACE VIEW`, and `ALTER VIEW` forms while another ownerless peer
remains open. Explicit view column lists were still covered only as crash/reopen
evidence: the killed writer stored or rewrote MariaDB's native view definition
file, but recovery of the alias metadata waited for no-live cleanup.

This slice promotes focused explicit column-list `CREATE VIEW`,
`CREATE OR REPLACE VIEW`, and `ALTER VIEW` forms to the metadata-only
live-recovery lane.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and `ALTER VIEW` accept
    `view_list_opt` before `AS view_select`.
  - `view_list` records explicit aliases in `Lex->view_list`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` validates explicit view-list arity, assigns each
    alias to the matching select item, and stores the view definition through
    `mysql_register_view()`.
  - `mysql_register_view()` writes the native view definition and refreshes
    table-definition cache state after successful registration.
- `packages/libmylite/src/database.cc`
  - Ownerless metadata-only dictionary recovery uses recoverable dictionary
    kinds for view DDL that does not require native file-operation checkpoint
    marker evidence.
  - The focused view recovery classifiers identify the supported SQL spellings
    at the `dictionary-before-finish` crash boundary.

## Design

Broaden the existing focused view recovery classifiers so they consume an
optional parenthesized column-list between the view identifier and `AS`.

The parser remains intentionally narrow:

- It only changes focused `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and
  `ALTER VIEW` recovery classification.
- It does not try to validate alias count or dependency correctness; MariaDB
  already completed the native statement before the crash hook fires.
- It does not change table DDL file-operation marker requirements.

Upgrade the three existing hook selectors so a peer stays live after the writer
is killed. A new ownerless opener must finish the metadata-only recovery while
that peer remains open, observe the alias metadata and active projection, and
prove the native file-operation marker remains clear.

## Scope

In scope:

- Focused explicit column-list `CREATE VIEW ... (aliases) AS ...` live
  recovery.
- Focused explicit column-list `CREATE OR REPLACE VIEW ... (aliases) AS ...`
  live recovery.
- Focused explicit column-list `ALTER VIEW ... (aliases) AS ...` live
  recovery.
- Alias metadata, ordinal-position, stale-column rejection, and query behavior
  checks while the peer remains live.
- Ownerless/native reopen and forced `.shm` rebuild after final peer release.

Out of scope:

- Invalid alias arity or dependency diagnostics.
- Check-option, nested, security/definer, and idempotent/no-op view variants.
- Trigger, schema, and broader metadata-only DDL live recovery.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL surface is newly enabled. The slice strengthens ownerless crash recovery
evidence for MariaDB-compatible explicit view column aliases by proving the
completed native metadata is visible to a new opener before all peers exit.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The live-recovery path consumes the
recoverable dictionary prefinish marker and updates ownerless dictionary state
without requiring `mylite-concurrency.ckpt` native file-operation marker
evidence. The final no-live close and forced `.shm` rebuild continue to verify
the recovered native view `.frm` state.

## Native Storage Impact

The base tables are InnoDB, but the promoted recovery class is metadata-only
view definition recovery. The slice does not change InnoDB page-version WAL,
redo/checkpoint policy, page flushing, or native tablespace file lifecycle.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-view-column-list-create-crash`
  - `dictionary-view-column-list-replace-crash`
  - `dictionary-view-column-list-alter-crash`
- Run the focused CTest entries for the three selectors.
- Run adjacent simple view metadata-only live-recovery selectors.
- Run production embedded ownerless view and broader DDL selectors.
- Run production-build guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A killed explicit column-list view writer is recovered by a new ownerless
  opener while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for all three
  metadata-only view forms.
- Create recovery exposes `view_id`, `view_value`, and `view_note` in ordinal
  order and queries the created view.
- Replacement recovery exposes `adjusted_value`, rejects `view_value`, and
  queries the replacement predicate/projection.
- Alter recovery exposes `doubled_value`, rejects `view_value`, and queries the
  altered predicate/projection.
- Ownerless and ordinary native reopen observe the same recovered state before
  and after forced `.shm` rebuild.

## Verification Results

- Hook build: `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`.
- Hook selectors:
  - `dictionary-view-column-list-create-crash`
  - `dictionary-view-column-list-replace-crash`
  - `dictionary-view-column-list-alter-crash`
- Hook CTest:
  `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-column-list-(create|replace|alter)-crash$' --output-on-failure`.
- Adjacent hook selectors:
  - `dictionary-view-create-crash`
  - `dictionary-view-replace-crash`
  - `dictionary-view-alter-crash`
- Production embedded build:
  `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`.
- Production selectors:
  - `view-ddl`
  - `ddl-broader`
  - `view-column-list`
- Targeted stress:
  - `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
  - `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- Guards:
  - `cmake --build --preset prod --target format-check`
  - `tools/check-ci-production-builds`
  - `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  - `git diff --check`
- Cleanup scan found no `/tmp/mylite-ownerless-*` directories and no matching
  ownerless test processes.

## Risks And Follow-Up

- This remains focused SQL-shape recovery, not a complete parser for every
  legal view definition spelling.
- Check-option, nested, security/definer, and idempotent/no-op view variants
  still need live-recovery promotion.
- Trigger, schema, and broader metadata-only DDL live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
