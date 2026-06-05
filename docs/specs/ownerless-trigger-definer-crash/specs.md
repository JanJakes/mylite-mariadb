# Ownerless Trigger Definer Crash

## Problem Statement

Ownerless trigger crash coverage preserves simple, replacement, ordering,
idempotent, and delayed-dependency trigger metadata after a writer dies after
MariaDB native `.TRG`/`.TRN` changes but before MyLite publishes ownerless
dictionary finish. One remaining bounded trigger metadata gap is an explicit
trigger definer. MariaDB stores the definer in trigger metadata and exposes it
through `INFORMATION_SCHEMA.TRIGGERS` and `SHOW CREATE TRIGGER`; MyLite must
recover that metadata at the same crash boundary.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `create_or_replace definer_opt
  TRIGGER_SYM`, and the `definer_opt` grammar distinguishes an omitted definer
  from an explicit `CURRENT_USER` definer.
- `mariadb/sql/sql_trigger.cc`
  `Table_triggers_list::create_trigger()` calls `sp_process_definer()` before
  native trigger file creation.
- `mariadb/sql/sql_trigger.cc` `build_trig_stmt_query()` appends `DEFINER=`
  to both the binary-log statement and stored trigger definition when the
  trigger is SUID/definer-backed.
- `mariadb/sql/sql_show.cc` stores trigger definer metadata for
  `INFORMATION_SCHEMA.TRIGGERS` and includes the stored statement in
  `SHOW CREATE TRIGGER`.

## Design

Add a hook-only selector,
`dictionary-trigger-definer-crash`, to
`mylite_ownerless_cross_process_sql_test`:

1. Create InnoDB base and audit tables.
2. Keep a live ownerless peer open.
3. Kill a writer after
   `CREATE DEFINER=CURRENT_USER TRIGGER ... AFTER INSERT ...` writes native
   trigger metadata but before ownerless dictionary finish.
4. Verify live-peer cleanup remains busy until the peer closes.
5. Reopen no-live ownerless and verify `.TRG`/`.TRN` files,
   `INFORMATION_SCHEMA.TRIGGERS.DEFINER`, `SHOW CREATE TRIGGER` containing
   `DEFINER=`, trigger firing, ownerless/native reopen, and forced `.shm`
   rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for an explicit
  `DEFINER=CURRENT_USER` trigger,
- recovered trigger definer metadata through information schema and
  `SHOW CREATE TRIGGER`,
- recovered `.TRG` and `.TRN` files under `datadir/app/`,
- post-recovery trigger firing and audit table persistence,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- privilege enforcement beyond MariaDB's embedded current-user semantics,
- stored functions or stored procedures called by triggers,
- randomized trigger crash oracle coverage,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens existing ownerless
trigger crash-recovery evidence by proving explicit definer metadata survives
the same dictionary publication crash boundary as ordinary trigger metadata.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native MariaDB trigger
metadata files under the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_definer_crash_base.TRG`,
- `datadir/app/ownerless_trigger_definer_crash_ai.TRN`.

## Native Storage Impact

Trigger metadata is MariaDB-native SQL-layer metadata. The base and audit
tables are InnoDB, so the selector also checks post-recovery DML and native
tablespace persistence, but it does not change InnoDB file formats,
page-version replay, redo/checkpoint policy, or file lifecycle routing.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-definer-crash`
- Run adjacent hook trigger crash selectors and `crash-tail`.
- Run adjacent embedded trigger selectors.
- Run ownerless SQL CTest filters, ownerless stress, `format-check`, `tidy`,
  and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata exposes the trigger through
  `INFORMATION_SCHEMA.TRIGGERS` with a non-empty definer, keeps `.TRG` and
  `.TRN` files present, and preserves `DEFINER=` in `SHOW CREATE TRIGGER`.
- The recovered trigger fires and persists base/audit rows.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This verifies explicit current-user definer metadata, not the full privilege
  model for arbitrary definers in a server account environment.
- Stored-function trigger bodies and randomized trigger crash variants remain
  planned.
- Full external MariaDB/RQG long-running DDL stress remains planned.
