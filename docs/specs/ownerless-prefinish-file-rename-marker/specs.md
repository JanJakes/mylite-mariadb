# Ownerless Prefinish File Rename Marker

## Problem Statement

Ownerless `RENAME TABLE` crash recovery already covers writers killed at the
`dictionary-before-finish` fault and verifies that reopen rebuilds the final
dictionary state. A narrower native-file lifecycle gap remains: MariaDB can log
InnoDB `FILE_RENAME` redo before MyLite reaches
`ownerless_finish_dictionary_ddl()`, but MyLite currently persists the native
file-operation checkpoint-needed marker only after dictionary finish returns.
If a hook-build writer is killed at `dictionary-before-finish`, the durable
marker can be absent even though uncheckpointed native file-rename redo exists.

This slice persists the existing native file-operation checkpoint-needed marker
as soon as ownerless dictionary finish observes native `FILE_RENAME` redo
evidence, before the `dictionary-before-finish` hook can stop the process.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:1607` implements
  `mtr_t::log_file_op()`. It calls
  `mylite_ownerless_innodb_note_file_rename_redo()` when `type ==
  FILE_RENAME`, before emitting the redo record.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:679`
  implements `mylite_ownerless_innodb_note_file_rename_redo()` and
  `mylite_ownerless_innodb_take_file_rename_redo()` as a process-local atomic
  evidence bit.
- `packages/libmylite/src/database.cc:14142` clears stale rename-redo evidence
  after `ownerless_begin_dictionary_ddl()` succeeds, so later evidence belongs
  to the current dictionary DDL statement.
- `packages/libmylite/src/database.cc:14152` pauses at
  `dictionary-before-finish` before the dictionary generation is finished.
- `packages/libmylite/src/database.cc:11099` implements
  `mark_ownerless_native_file_op_checkpoint_after_dictionary_ddl()`, which
  currently consumes `FILE_RENAME` evidence only after successful dictionary
  finish in the prepared path (`database.cc:3704`) and direct SQL path
  (`database.cc:5076`).
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c:37614` already
  has a `RENAME TABLE` writer killed at `dictionary-before-finish`, but it does
  not assert that the native file-op marker exists before reopen recovery can
  drain it.

## Scope And Non-Goals

In scope:

- Persist the existing native file-op checkpoint-needed marker before the
  `dictionary-before-finish` hook when native `FILE_RENAME` redo evidence is
  present.
- Preserve the current after-finish marker behavior for DDL classes that are
  detected from SQL policy tokens, such as `ALTER TABLE ... AUTO_INCREMENT`.
- Add a focused hook-build selector for the `RENAME TABLE` crash window and
  register it as a CTest.
- Update ownerless concurrency and compatibility documentation.

Out of scope:

- Adding a durable DDL journal.
- Broadening file-op marker policy to every `CREATE`, `DROP`, `TRUNCATE`, or
  non-rename `ALTER` shape.
- Changing MariaDB redo record format, file-operation recovery parsing, or
  checkpoint suppression.
- Claiming the broader DDL/file-lifecycle recovery matrix is complete.

## Design

Add a MyLite helper that consumes only native `FILE_RENAME` redo evidence and
marks `mylite-concurrency.ckpt` as needing a native file-operation checkpoint.
Call it inside `ownerless_finish_dictionary_ddl()` after confirming a
dictionary DDL was started and before the `dictionary-before-finish` test hook.

The helper deliberately does not attempt `mylite_ownerless_innodb_make_checkpoint()`
before dictionary finish. The product requirement for this window is durable
evidence that later no-live ownerless startup/close must use the existing
uncheckpointed file-operation recovery bridge and final native checkpoint
drain. Normal successful statements still proceed to dictionary finish and the
existing after-finish helper remains responsible for token-detected marker
classes.

## Affected Subsystems

- First-party ownerless dictionary DDL coordination in
  `packages/libmylite/src/database.cc`.
- Existing MyLite InnoDB ownerless file-rename redo hook in
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`.
- Existing InnoDB `FILE_RENAME` redo emission in
  `mariadb/storage/innobase/fil/fil0fil.cc`.
- Hook-only ownerless cross-process SQL crash tests.

## Compatibility Impact

No SQL result or public C API behavior changes. The slice strengthens crash
recovery evidence for ownerless `RENAME TABLE` in the embedded ownerless
profile while preserving MariaDB's native rename semantics.

## DDL Metadata Routing Impact

The dictionary-generation protocol is unchanged. The marker is written before
dictionary finish only when native `FILE_RENAME` redo was observed during the
same dictionary DDL statement. Dirty dictionary state still blocks peer cleanup
until no-live recovery rebuilds or finishes the state.

## Directory And Lifecycle Impact

The slice reuses the existing `concurrency/mylite-concurrency.ckpt`
native-file-op marker records. A crash before dictionary finish can now leave a
durable marker that later ownerless/native startup treats as evidence that
uncheckpointed file-operation recovery may be required. Final no-live close
continues to drain and clear the marker through the existing native checkpoint
path.

## Native Storage Impact

No native file format changes. The slice records MyLite-owned recovery evidence
for MariaDB's native `FILE_RENAME` redo boundary and leaves InnoDB redo parsing
unchanged.

## Build, Size, License, And Dependencies

No new dependencies, license impact, public symbols, or binary-size-sensitive
build-profile changes. The CTest registration is hook-build only.

## Test And Verification Plan

- Build the hook test binary:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`.
- Run the new focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-rename-file-op-marker-crash`.
- Run adjacent rename and marker selectors:
  `dictionary-rename-crash`, `dictionary-cross-schema-rename-crash`, and
  `native-file-op-marker-drain`.
- Run hook CTest coverage for the new CTest and adjacent selectors.
- Run production focused replay selectors that exercise renamed tablespace
  recovery and the native marker drain.
- Run `tools/check-ci-production-builds`, format check, and `git diff --check`.

## Verification Results

- `cmake --preset ownerless-test-hooks` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  dictionary-rename-file-op-marker-crash` passed.
- Adjacent hook selectors passed:
  `dictionary-rename-crash`, `dictionary-cross-schema-rename-crash`, and
  `native-file-op-marker-drain`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-dictionary-rename-file-op-marker-crash|ownerless-native-file-op-marker-drain|ownerless.*rename.*crash)'
  --output-on-failure` passed the registered new CTest 1/1 in 2.00 seconds
  after the marker assertion was placed in the RENAME crash test.
- `cmake --preset php-embedded-prod` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Production selectors passed:
  `renamed-tablespace-replay`, `rename-create-tablespace-replay`, and
  `native-file-op-marker-drain`.
- `tools/check-ci-production-builds` passed with
  `ci_production_build_audit_ok=.github/workflows/ci.yml`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after `RENAME TABLE` leaves
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the held peer is released.
- The same scenario still recovers the renamed table through ownerless and
  ordinary native reopen.
- Existing native file-op marker drain coverage still clears stale and real SQL
  markers.
- Docs continue to mark broader DDL/file-lifecycle recovery as partial.

## Risks And Follow-Up

- The hook proves the `FILE_RENAME` crash window, not every DDL file operation.
- SQL-level table-lock fault injection remains unproven and should stay
  documented separately.
- Broader native redo/checkpoint reconciliation and DDL/file lifecycle recovery
  still need additional slices.
