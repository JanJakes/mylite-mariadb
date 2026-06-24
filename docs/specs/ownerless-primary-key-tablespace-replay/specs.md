# Ownerless Primary-Key Tablespace Replay

## Problem Statement

Ownerless stale-reader tablespace replay covers several file-per-table DDL
shapes, and primary-key replacement already has peer-refresh plus hook-build
crash recovery coverage. The remaining replay gap is the no-live recovery path
after a stale reader pins pre-rebuild WAL while a writer replaces the clustered
primary key. This matters because stale page images from the old clustered-key
layout must not overwrite the final table after `.shm` rebuild.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:7508-7535` classifies dropped and added primary
  keys as `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX` in the storage-engine
  ALTER flags.
- `mariadb/storage/innobase/handler/handler0alter.cc:2365-2372` documents and
  enforces InnoDB's rule that `DROP PRIMARY KEY` is only accepted together with
  `ADD PRIMARY KEY`.
- `mariadb/storage/innobase/handler/handler0alter.cc:4115-4166` builds new
  InnoDB index definitions with a new primary key first, and
  `mariadb/storage/innobase/handler/handler0alter.cc:5019-5056` compares the
  old and new clustered primary-key order during ALTER planning.
- Existing ownerless primary-key refresh and crash tests already prove
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)` metadata,
  duplicate enforcement, and ownerless dictionary crash recovery.

## Design

Add `test_ownerless_primary_key_tablespace_replay_keeps_replacement_key`:

1. Create `app.ownerless_primary_key_replay` with `PRIMARY KEY (id)` and
   unique `code` values, then checkpoint the clean baseline.
2. Start a repeatable-read ownerless peer to pin the pre-rebuild snapshot.
3. Update the original rows so retained page-version WAL exists while the
   stale reader remains live.
4. Run `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code)`.
5. Insert a row that duplicates the old `id` but has a unique `code`, and
   verify a duplicate `code` insert fails.
6. Kill the stale reader and verify ownerless/native reopen, before and after
   forced `.shm` rebuild, preserve the final clustered-key metadata and rows.

Verification of the adjacent `primary-key-ddl` selector exposed a failed local
ownerless dictionary-DDL cleanup gap: after a parent-side rejected
`ALTER TABLE ... ADD PRIMARY KEY (code)`, a later peer primary-key replacement
could leave the parent observing stale primary-key metadata. The implementation
therefore keeps rollback-fenced handles eligible for dictionary-generation
refresh before suppressing page-version/native page refresh, and also flushes
local SQL/InnoDB dictionary caches after failed ownerless dictionary DDL once
the shared dictionary generation has been finished, while preserving the
original MariaDB error diagnostics.

## Scope And Non-Goals

In scope:

- One representative primary-key replacement stale-reader replay case.
- Final primary-key metadata on `code`.
- Duplicate enforcement on the new primary key and duplicate allowance on the
  former primary-key column.
- Ownerless/native reopen and forced `.shm` rebuild.

Out of scope:

- Descending, composite-direction, and AUTO_INCREMENT primary-key replacement
  replay variants.
- New production recovery logic unless the focused test exposes a bug.
- External MariaDB/RQG randomized DDL stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice adds recovery evidence for a
MariaDB-valid clustered-key replacement while older page-version WAL is
retained by a live snapshot reader, and preserves existing MariaDB diagnostics
for rejected local dictionary DDL while making later peer dictionary refresh
observe the final metadata. Rollback-fenced failed writes still avoid
ownerless page-version/native page refresh until later local visibility
advancement clears the fence.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The final `.frm` and `.ibd` remain
inside the MyLite database directory, and the test verifies native page-0
tablespace identity after no-live recovery and forced `.shm` rebuild.

## Native Storage Impact

Native InnoDB remains responsible for the clustered-key rebuild. MyLite
recovery must preserve the final rebuilt native table and avoid replaying stale
pre-rebuild page images over it.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
diff stays within first-party ownerless refresh logic, SQL coverage, and
documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production selector:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-tablespace-replay`.
- Run the focused production case-table route:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_primary_key_tablespace_replay_keeps_replacement_key`.
- Run adjacent primary-key and force-rebuild selectors.
- Build and run the focused selector and adjacent primary-key DDL selector
  under `ownerless-test-hooks`.
- Build `ownerless-stress` and run the DDL stress selector.
- Run `format-check-prod`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- The focused selector leaves retained page-version WAL while the stale reader
  is live and the primary-key replacement has completed.
- After the stale reader is killed, ownerless reopen checkpoints retained WAL
  and preserves the rebuilt primary-key state.
- Ordinary native reopen and forced `.shm` rebuild show `PRIMARY(code)`,
  no `PRIMARY(id)`, duplicate `code` rejection, duplicate old-`id`
  preservation, expected aggregates, and page-0 identity.

## Risks And Follow-Up

- This is representative clustered-key replay coverage, not a full
  primary-key replacement matrix.
- Broader native redo/checkpoint reconciliation, additional DDL
  file-lifecycle crash windows, transaction rollback crash windows,
  active-reader pressure crash/oracle breadth, and external MariaDB/RQG stress
  remain open completion work.
