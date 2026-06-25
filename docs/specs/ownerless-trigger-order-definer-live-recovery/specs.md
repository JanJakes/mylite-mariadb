# Ownerless Trigger Order Definer Live Recovery

## Problem Statement

Ownerless trigger crash recovery already handles simple trigger create/drop,
replacement, idempotent/no-op, delayed missing-dependency, and stored-function
body trigger metadata while another ownerless peer remains live. The remaining
bounded trigger crash selectors for ordered trigger insertion and explicit
current-user definer creation used the older no-live path even though both write
only MariaDB native `.TRG` and `.TRN` trigger metadata after native SQL
completion.

MyLite should recover these two trigger metadata boundaries while a peer remains
live, keep the native file-operation checkpoint marker clear, and preserve the
existing checks for `ACTION_ORDER`, firing order, definer metadata, and final
ownerless/native reopen.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses trigger DDL as
  `create_or_replace definer_opt TRIGGER_SYM trigger_tail`; `definer_opt`
  accepts `DEFINER = user_or_role`, and `trigger_tail` parses optional
  `FOLLOWS` or `PRECEDES` after `FOR EACH ROW`.
- `mariadb/sql/sql_trigger.cc` `build_trig_stmt_query()` stores explicit
  `DEFINER=` in the persisted trigger definition and strips the original
  ordering clause from stored trigger text, so ordered trigger state is carried
  by trigger-list `action_order`.
- `mariadb/sql/sql_trigger.cc`
  `Table_triggers_list::create_trigger()` calls `sp_process_definer()`, checks
  that an ordering anchor exists with the same event and timing, creates or
  replaces the trigger-name `.TRN` file, inserts the trigger into the in-memory
  list, recomputes `action_order`, and writes the table-level `.TRG` file.
- Existing MyLite dictionary recovery already treats
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER` as metadata-only when a
  dead owner can be recovered while live peers remain open.

## Design

- Extend `ownerless_create_trigger_recovery_statement()` to accept the same
  bounded trigger metadata class already covered by tests:
  - optional `DEFINER=CURRENT_USER` or `DEFINER=CURRENT_USER()` before
    `TRIGGER`,
  - one optional `FOLLOWS <trigger>` or `PRECEDES <trigger>` clause immediately
    after `FOR EACH ROW`.
- Keep arbitrary definers, privilege/security semantics, invalid ordering
  anchors, and broader randomized trigger matrices out of this slice.
- Reuse `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TRIGGER`; no dictionary
  marker format or directory layout changes are required.
- Promote `dictionary-trigger-order-crash` and
  `dictionary-trigger-definer-crash` to the held-live-peer recovery helper.
  Each selector asserts the native file-operation checkpoint marker remains
  clear before and after the live recovery checks.
- Register `dictionary-trigger-order-crash` as a standalone hook CTest so both
  promoted trigger variants have visible timing and failure attribution.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless crash-recovery
compatibility for MariaDB-native trigger metadata that was already covered by
no-live crash selectors and SQL-level ownerless trigger behavior tests.

## Directory And Lifecycle Impact

No directory layout changes. The recovered state remains MariaDB trigger
metadata under `datadir/app/` plus existing ownerless dictionary state under the
MyLite concurrency directory. The tests keep existing ownerless/native reopen
coverage before and after forced `.shm` rebuild.

## Native Storage Impact

Trigger metadata is SQL-layer native metadata. The base and audit tables are
InnoDB and continue to validate post-recovery DML effects, but this slice does
not change InnoDB redo/checkpoint policy, page-version WAL, file-per-table
recovery, or native storage formats.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run focused hook CTests for primitives and trigger crash selectors:
  `dictionary-trigger-order-crash`, `dictionary-trigger-definer-crash`, and
  adjacent create/drop, replacement, idempotent, invalid-dependency, and
  stored-function trigger crash selectors.
- Run production embedded trigger selectors: `trigger-ddl`,
  `trigger-ddl-variants`, `trigger-ordering`, `trigger-idempotent-ddl`, and
  routine policy selectors.
- Run ownerless DDL stress because the metadata-only trigger classifier
  changed.
- Run `format-check`, CI production-build guards, production guard CTest, and
  `git diff --check`.

## Acceptance Criteria

- A killed ordered trigger writer recovers while another ownerless peer remains
  live, keeps the native file-operation marker clear, preserves `.TRG` and all
  `.TRN` files, exposes recovered `ACTION_ORDER`, and fires triggers in the
  recovered order.
- A killed explicit `DEFINER=CURRENT_USER` trigger writer recovers while
  another ownerless peer remains live, keeps the native file-operation marker
  clear, preserves non-empty `INFORMATION_SCHEMA.TRIGGERS.DEFINER`, preserves
  `DEFINER=` in `SHOW CREATE TRIGGER`, and fires the recovered trigger.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
  passed.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-dictionary-trigger-(create|drop|replace|order|definer|idempotent-create|idempotent-drop|invalid-dependency|stored-function)-crash$'
  --output-on-failure` passed for the registered primitive, definer, order,
  replacement, and idempotent trigger crash CTests.
- Direct hook selectors passed:
  `dictionary-trigger-create-crash`, `dictionary-trigger-drop-crash`,
  `dictionary-trigger-invalid-dependency-crash`, and
  `dictionary-trigger-stored-function-crash`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Production selectors passed: `trigger-ddl`, `trigger-ddl-variants`,
  `trigger-ordering`, `trigger-idempotent-ddl`, `routine-policy`, and
  `routine-execution-policy`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed on clean rerun: 1/1 in 9.39 seconds. An earlier run against a temp
  directory that later showed InnoDB undo LSN corruption timed out at 900
  seconds; no ownerless test processes remained, the temp directory was
  removed, and the clean rerun passed.
- `cmake --build --preset prod --target format-check` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed: 1/1 in 3.48 seconds.
- `git diff --check` passed.

## Risks And Follow-Up

- This proves current-user definer metadata, not arbitrary account definers or
  a server privilege model.
- This proves one ordered insertion over existing same-event/same-timing
  triggers, not invalid-anchor or randomized trigger ordering matrices.
- Broader metadata-only classes, native redo/checkpoint reconciliation, active
  reader pressure policy, and external MariaDB/RQG-style stress remain planned.
