# Ownerless FK Rename-Loop Skip Coverage

## Problem

Ownerless FK rename-loop crash coverage now proves first native-pair rollback
for same-schema and cross-schema `RENAME TABLE` chains. The remaining native
rename-loop gap is later-pair fault coverage: a writer can die after the second
or final successful native rename pair but before MariaDB advances the DDL-log
phase past the table-rename step.

This slice makes the later-pair evidence explicit in CI by reusing the existing
unsafe `rename-table-after-native-file-op` fault and the existing rollback
assertions with deterministic skip counts.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `do_rename()` now calls `mylite_ownerless_innodb_test_fault()` after each
    successful native `mysql_rename_table()` call and before the DDL-log phase
    advances to trigger/statistics handling.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_test_fault()` honors
    `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, counting matching fault hits before
    signalling the ready fd and pausing/crashing.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `dictionary-fk-multi-rename-loop-crash` asserts same-schema rollback to
    original parent/child FK state.
  - `dictionary-fk-cross-schema-multi-rename-loop-crash` asserts cross-schema
    rollback to original `app` parent/child FK state with the target schema
    retained and moved tables absent.

## Scope And Non-Goals

In scope:

- Register standalone hook CTests that run the existing same-schema FK
  rename-loop selector with:
  - `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`, killing after the child rename pair;
  - `MYLITE_OWNERLESS_TEST_FAULT_SKIP=2`, killing after the final parent rename
    pair in the temporary-parent chain.
- Register a standalone hook CTest that runs the existing cross-schema FK
  rename-loop selector with `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`, killing after
  the child rename pair.
- Update compatibility docs to move later-pair FK native-loop coverage from
  planned to covered for these deterministic FK chains.

Out of scope:

- Arbitrary skip counts outside the covered statement lengths.
- Non-FK rename-list matrices and mixed temporary/permanent later-pair lists.
  Existing-table FK `IF EXISTS` rename-loop variants are covered by the
  `ownerless-fk-rename-if-exists-list-loop-coverage` follow-up.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external randomized DDL/RQG
  stress.
- SQL-level table-lock wait fault injection.

## Design

- Do not add new production code.
- Reuse the current hook-only selectors and their rollback assertions.
- Add three CTest registrations with the same command but distinct names and
  `ENVIRONMENT "MYLITE_OWNERLESS_TEST_FAULT_SKIP=<n>"`.
- Keep labels and timeout aligned with adjacent FK rename-loop crash tests so
  CI timing remains visible and comparable.

## Compatibility Impact

No SQL syntax, C API, storage format, or runtime behavior changes. The slice
adds compatibility evidence that MariaDB DDL-log rollback remains authoritative
for later native-pair crashes in the covered FK rename chains.

## Directory And Lifecycle Impact

No directory layout changes. The reused selectors verify ownerless live-peer
recovery, marker retention/drain, ownerless/native reopen, forced `.shm`
rebuild, native files, and FK enforcement after rollback.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB dictionary
rollback remain the authority for the observed outcomes.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Configure `ownerless-test-hooks` so the new CTests are registered.
- Run the three new standalone CTests.
- Run the adjacent FK rename/drop loop hook subset.
- Run production non-hook FK multi-rename selectors.
- Run format and diff checks.

## Acceptance Criteria

- Same-schema skip=1 and skip=2 CTests pass with the original-state rollback
  assertions.
- Cross-schema skip=1 CTest passes with the original-state rollback assertions.
- The CTests have separate names and timing in CI.
- Docs no longer list deterministic later-pair FK rename-loop points as
  planned.

## Risks And Open Questions

- This proves deterministic ordinary FK chains only. Existing-table FK
  `IF EXISTS` variants are covered by the
  `ownerless-fk-rename-if-exists-list-loop-coverage` follow-up; broader mixed
  rename matrices and randomized DDL/RQG oracles remain completion work.
