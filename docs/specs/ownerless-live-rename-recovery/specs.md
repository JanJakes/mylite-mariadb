# Ownerless Live Rename Recovery

## Problem Statement

The CREATE-family prefinish crash boundaries now have live ownerless recovery
after the durable native file-operation marker is published. The next DDL file
lifecycle class is `RENAME TABLE`, where MariaDB moves the native table files
and dictionary identity before MyLite reaches `dictionary-before-finish`.

Existing hook coverage proved same-schema, cross-schema, and multi-pair rename
after no-live recovery. This slice upgraded only the conservative single-pair
same-schema form, `RENAME TABLE schema.table TO schema.other_table`, so a live
ownerless opener could finish the dead dictionary generation while another peer
remained open. The follow-up
`docs/specs/ownerless-live-rename-list-recovery/specs.md` now extends that
live recovery to explicit schema-qualified rename lists.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_RENAME_TABLE` calls `check_rename_table()` and
    `mysql_rename_tables()`.
  - Rename permission checks walk old/new pairs through `next_local`.
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` takes table-name locks and delegates the ordered
    pair list to `rename_tables()`.
  - `rename_tables()` writes `ddl_log_rename_table()`, calls
    `mysql_rename_table()`, advances trigger/stat-table phases, and completes
    the DDL log only after successful rename work.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` performs the storage-engine/native file rename and
    handler metadata update.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the native file-op checkpoint
    marker before `dictionary-before-finish` and records a recoverable
    dictionary marker only when InnoDB observed native file-operation redo.
  - Dead-owner dictionary recovery requires the durable file-op marker plus the
    existing idle native-state gates before it can finish an odd dictionary
    generation while peers remain live.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for single-pair same-schema
  `RENAME TABLE schema.table TO schema.other_table`.
- Require explicit matching schemas and reject multi-pair or cross-schema
  rename by classifier shape.
- Convert the focused same-schema rename hook selector to prove live recovery,
  source absence, target presence, post-recovery writes, live marker retention,
  no-live marker drain, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Implicit-schema rename names.
- `RENAME TABLE IF EXISTS` and temporary-table rename.
- View-only rename classes.
- Trigger-bearing rename classes.
- Truncate, drop, rebuild, schema, view, trigger, partition/import/export, or
  metadata-only DDL live recovery.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state already stores recovery kind, owner id, and owner
generation, so no `.shm` layout change is required. Add
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE`, accept it in primitive
validation, and include it in dead-owner cleanup's recovery-kind probe list.

`ownerless_dictionary_recovery_kind_for_statement()` adds a conservative raw
token classifier for exactly `RENAME TABLE schema.table TO schema.table`. It
requires the raw token sequence to contain explicit `schema . table` pairs,
requires the schemas to normalize to the same value, and allows only a trailing
semicolon after the target. Cross-schema and multi-pair rename therefore remain
on the existing no-live recovery path until focused live tests are added.

Live recovery only finishes the dictionary generation. The native file-op
marker remains set while any peer is live and is drained by the existing final
no-live checkpoint path.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after a same-schema single-pair `RENAME TABLE` can
now be recovered by a live ownerless opener. The source table is absent, the
target table is present and writable, and the final state remains durable
through ownerless/native reopen.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native `.frm`/`.ibd` rename as the storage authority and keeps the MyLite
native file-op checkpoint-needed marker durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-cross-schema-rename-crash`, and the adjacent create/replacement,
  truncate, and drop marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-rename-file-op-marker-crash`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Same-schema rename crash coverage no longer expects `MYLITE_BUSY` while a
  peer is live.
- A live ownerless opener recovers the dead rename writer, observes source
  absence and target presence, inserts a row, and verifies target aggregates.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the marker
  and preserves rename state through ownerless/native reopen and forced `.shm`
  rebuild.
- Cross-schema, multi-pair, and foreign-key parent/child rename-list selectors
  are covered by
  `docs/specs/ownerless-live-rename-list-recovery/specs.md`.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-rename-file-op-marker-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-cross-schema-rename-crash`
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-table-crash|create-or-replace-after-drop-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-rename-file-op-marker-crash`
- `cmake --build --preset prod --target format-check`
- `git diff --check`
- `find /tmp -maxdepth 1 -type d -name 'mylite-ownerless-*' -print`
- `pgrep -af '([m]ylite_ownerless|[o]wnerless_cross_process|[o]wnerless_primitives)' || true`

## Risks And Follow-Up

- Implicit-schema, `IF EXISTS`, temporary-table, and view-only rename forms
  require separate coverage before being considered live-recoverable.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
