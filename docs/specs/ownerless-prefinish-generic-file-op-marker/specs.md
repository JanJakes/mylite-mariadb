# Ownerless Prefinish Generic File-Op Marker

## Problem Statement

Ownerless `RENAME TABLE` now persists the native file-operation
checkpoint-needed marker before the `dictionary-before-finish` test hook when
InnoDB has emitted `FILE_RENAME` redo. The underlying marker is not
rename-specific: it records that a native file-operation boundary may require
uncheckpointed native startup recovery and a final native checkpoint drain.

`TRUNCATE TABLE` and other DDL can emit different InnoDB `FILE_*` redo records
before MyLite reaches dictionary finish. If a hook-build writer is killed in
that same window, durable ownerless evidence should not depend only on the
`FILE_RENAME` class.

This slice generalizes the prefinish redo-evidence bit from `FILE_RENAME` to
any logged InnoDB `FILE_CREATE`, `FILE_DELETE`, `FILE_MODIFY`, or
`FILE_RENAME` record and proves the broader boundary with a focused
`TRUNCATE TABLE` crash selector.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:1607` implements
  `mtr_t::log_file_op()` for native tablespace file-operation redo.
- The same file emits `FILE_DELETE` from `fil_delete_tablespace()` around
  `fil0fil.cc:1734`, `FILE_RENAME` from `fil_space_t::rename()` around
  `fil0fil.cc:2016`, and `FILE_CREATE` from `fil_ibd_create()` around
  `fil0fil.cc:2098`.
- `mariadb/storage/innobase/log/log0recv.cc:2803` parses
  `FILE_DELETE`, `FILE_MODIFY`, `FILE_RENAME`, and `FILE_CREATE` records
  through the same file-name recovery path.
- `packages/libmylite/src/database.cc` consumes the file-operation evidence in
  ownerless dictionary finish and persists
  `concurrency/mylite-concurrency.ckpt` marker records before the
  `dictionary-before-finish` hook can stop the process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  `dictionary-truncate-crash` recovery coverage. It can be reused with one
  marker assertion before peer release and reopen recovery.

## Scope And Non-Goals

In scope:

- Add generic MyLite InnoDB hook helpers for native file-operation redo
  evidence.
- Keep the existing rename helper names as wrappers for existing call sites and
  tests.
- Mark the native file-op checkpoint-needed boundary before dictionary finish
  when any logged native `FILE_*` redo was observed during the current
  ownerless dictionary DDL.
- Add a hook-only `dictionary-truncate-file-op-marker-crash` selector and CTest
  that asserts the marker exists after a killed truncate writer and before
  no-live recovery can drain it.

Out of scope:

- Changing InnoDB redo format or MyLite checkpoint file format.
- Implementing a complete durable DDL journal.
- Claiming complete recovery for every DDL file-lifecycle class.
- SQL-level table-lock fault injection.
- Optimizing ordinary DML write publication.

## Design

The existing process-local atomic evidence bit is generalized from rename to
file operation. `mtr_t::log_file_op()` sets that bit for every logged
`FILE_*` record after confirming the mini-transaction is logging redo. MyLite's
ownerless dictionary DDL begin path clears stale evidence, the prefinish path
consumes current evidence and writes the existing native file-op checkpoint
marker, and the after-finish path keeps the current behavior of attempting a
native checkpoint before falling back to the durable marker.

If the prefinish marker write fails, MyLite restores the generic evidence bit
so the after-finish path can still attempt the existing checkpoint or marker
fallback. The rename-specific wrapper functions remain available and operate
on the same generic bit.

## Compatibility Impact

No SQL result, public C API, or native storage format changes. The slice
strengthens crash-recovery evidence for ownerless DDL statements that emit
native InnoDB file-operation redo before dictionary finish.

## Directory And Lifecycle Impact

The slice reuses the existing
`concurrency/mylite-concurrency.ckpt` native-file-op marker records. A crash
after native `FILE_*` redo but before ownerless dictionary finish can now leave
the durable marker. Later no-live ownerless/native startup already treats that
marker as evidence to arm uncheckpointed file-operation recovery and final
native checkpoint draining.

## Native Storage Impact

No new native file format, redo record, or checkpoint record is introduced.
The change only broadens when MyLite records its own durable recovery evidence
for MariaDB-native file-operation redo that already exists.

## Build, Size, License, And Dependencies

No new dependencies, license impact, public symbols, or binary-size-sensitive
build-profile changes. The new CTest is hook-build only.

## Test And Verification Plan

- Build hook targets:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_embedded_ownerless_innodb_lock_hooks_test -j2`.
- Run primitive hook coverage:
  `ctest --preset ownerless-test-hooks -R libmylite.embedded-ownerless-innodb-lock-hooks --output-on-failure`.
- Run the new focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-truncate-file-op-marker-crash`.
- Run adjacent crash selectors:
  `dictionary-truncate-crash`, `dictionary-rename-file-op-marker-crash`, and
  `native-file-op-marker-drain`.
- Run focused hook CTest for the registered marker tests.
- Run production focused marker drain and truncated-tablespace replay selectors.
- Run production build audit, format check, and `git diff --check`.

## Verification Results

- `tools/mariadb-embedded-build build` passed after the InnoDB file-op hook
  change and refreshed `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --preset ownerless-test-hooks` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test
  mylite_embedded_ownerless_innodb_lock_hooks_test -j2` passed.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.embedded-ownerless-innodb-lock-hooks' --output-on-failure`
  passed 1/1.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  dictionary-truncate-file-op-marker-crash` passed.
- Adjacent hook selectors passed: `dictionary-truncate-crash`,
  `dictionary-rename-file-op-marker-crash`, and
  `native-file-op-marker-drain`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-dictionary-(rename|truncate)-file-op-marker-crash|libmylite\.embedded-ownerless-innodb-lock-hooks'
  --output-on-failure` passed 3/3.
- `cmake --preset php-embedded-prod` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test
  mylite_embedded_ownerless_innodb_lock_hooks_test -j2` passed.
- Production focused selectors passed:
  `mylite_embedded_ownerless_innodb_lock_hooks_test`,
  `native-file-op-marker-drain`, and `truncated-tablespace-replay`.
- `tools/check-ci-production-builds` passed with
  `ci_production_build_audit_ok=.github/workflows/ci.yml`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Generic file-operation hook evidence is set and consumed for low-level
  primitive tests while existing rename wrapper behavior is preserved.
- A writer killed at `dictionary-before-finish` after `TRUNCATE TABLE` leaves
  `read_concurrency_native_file_op_checkpoint_needed(database_path)` true
  before the live peer is released.
- The same truncate crash scenario still recovers the empty table and supports
  ownerless/native reopen after forced `.shm` rebuild.
- Docs continue to mark broader DDL/file-lifecycle recovery as partial.

## Risks And Follow-Up

- The original focused proof covers `TRUNCATE TABLE`, and the follow-up
  `ownerless-drop-file-op-marker-crash` slice covers a representative
  `DROP TABLE` `FILE_DELETE` boundary. A later
  `ownerless-create-like-file-op-marker-crash` slice covers a representative
  `CREATE TABLE ... LIKE` `FILE_CREATE` boundary, and a later
  `ownerless-ctas-file-op-marker-crash` slice covers populated CTAS
  `FILE_CREATE`. A later
  `ownerless-replacement-copy-file-op-marker-crash` slice covers representative
  `CREATE OR REPLACE TABLE ... LIKE` and
  `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy boundaries. Other
  later evidence covers representative `ALTER TABLE ... FORCE` and
  `ALTER TABLE ... ROW_FORMAT=DYNAMIC` rebuild boundaries. Other create-style,
  rebuild/replacement, `FILE_MODIFY`, and multi-file DDL combinations still
  need additional evidence before the broader DDL lifecycle claim can be
  upgraded.
- The native startup recovery mode is still named for rename because the
  original bridge was introduced for uncheckpointed file rename recovery.
  Renaming that API would be mechanical churn and is left out of this slice.
- Broader native redo/checkpoint reconciliation and external MariaDB/RQG stress
  remain planned.
