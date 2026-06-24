# Ownerless Live Create Or Replace Like Recovery

## Problem Statement

Plain create, table-copy `CREATE TABLE ... LIKE`, focused CTAS, and ordinary
`CREATE OR REPLACE TABLE` prefinish crash boundaries can be recovered by a live
ownerless peer after the durable native file-operation marker is published.
Replacement-copy `CREATE OR REPLACE TABLE ... LIKE` is the next bounded
same-name replacement class: MariaDB removes the existing target table, copies
the source table definition, creates a fresh empty target, then reaches
MyLite's `dictionary-before-finish` hook.

Existing hook coverage proved the copied-shape replacement after no-live
recovery. This slice upgrades only the conservative non-temporary
`CREATE OR REPLACE TABLE ... LIKE` form so a live ownerless opener can finish
the dead dictionary generation while another peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `Sql_cmd_create_table_like::execute()` dispatches regular LIKE-copy DDL to
    `mysql_create_like_table()` when the statement has no SELECT list.
  - `mysql_create_like_table()` opens the source table, derives the target
    `Table_specification_st` and `Alter_info` from source metadata, preserves
    the statement's `OR REPLACE` option, and calls
    `mysql_create_table_no_lock()` for the target creation.
- `mariadb/sql/sql_yacc.yy`
  - `LIKE table_ident` and parenthesized `(LIKE table_ident)` spellings are
    grammar alternatives. This slice covers the unparenthesized form used by
    existing ownerless replacement-copy tests.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the native file-op checkpoint
    marker before `dictionary-before-finish` and records a recoverable
    dictionary marker only when InnoDB observed native file-operation redo.
  - Dead-owner dictionary recovery requires the durable file-op marker plus the
    existing idle native-state gates before it can finish an odd dictionary
    generation while peers remain live.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for non-temporary
  `CREATE OR REPLACE TABLE ... LIKE`.
- Classify the conservative unparenthesized LIKE-copy replacement form and keep
  CTAS, temporary tables, views, routines, schemas, and parenthesized LIKE out.
- Convert the focused replacement-LIKE hook selector to prove live recovery,
  copied replacement metadata, old-column/index absence, post-recovery writes,
  live marker retention, no-live marker drain, ownerless/native reopen, and
  forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE TABLE ... AS SELECT` populated replacement recovery.
- Parenthesized `(LIKE source)` replacement-copy syntax.
- The `create-or-replace-after-drop` absence boundary.
- Rename, truncate, drop, rebuild, schema, view, trigger, foreign-key multi-DDL,
  partition/import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state already stores recovery kind, owner id, and owner
generation, so no `.shm` layout change is required. Add
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_LIKE`, accept it
in primitive validation, and include it in dead-owner cleanup's recovery-kind
probe list.

`ownerless_dictionary_recovery_kind_for_statement()` keeps the existing ordinary
replacement classifier rejecting `LIKE`. A new classifier accepts identifier
streams beginning with `CREATE OR REPLACE TABLE`, rejects `TEMPORARY` and
`SELECT`, rejects non-table CREATE object classes before the `TABLE` keyword,
then requires `LIKE` after `TABLE` with no raw `(` before it. That mirrors the
existing conservative table-copy classifier and avoids overclaiming CHECK or
parenthesized grammar cases.

Live recovery only finishes the dictionary generation. The native file-op
marker remains set while any peer is live and is drained by the existing final
no-live checkpoint path.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after `CREATE OR REPLACE TABLE ... LIKE` can now be
recovered by a live ownerless opener. The replacement table uses MariaDB's
native copied metadata, stays empty as MariaDB LIKE-copy semantics require, and
accepts post-recovery writes. Populated replacement CTAS and broader DDL
file-lifecycle recovery remain partial.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native replacement `.frm` and `.ibd` files as the storage authority and keeps
the MyLite native file-op checkpoint-needed marker durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-create-or-replace-like-file-op-marker-crash`,
  `dictionary-create-or-replace-table-crash`, and adjacent replacement CTAS,
  after-drop, create-table, create-like, CTAS, rename, truncate, and drop
  marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-create-or-replace-like-file-op-marker-crash`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Replacement-LIKE crash coverage no longer expects `MYLITE_BUSY` while a peer
  is live.
- A live ownerless opener recovers the dead replacement-LIKE writer, observes
  the copied empty target, verifies old-column/index absence and copied
  column/index presence, inserts rows, and verifies forced-index reads.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the marker
  and preserves replacement state through ownerless/native reopen and forced
  `.shm` rebuild.
- Replacement CTAS and after-drop replacement selectors remain no-live recovery
  guards.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-like-file-op-marker-crash`
  passed.
- Adjacent direct hook selectors
  `dictionary-create-or-replace-like-crash`,
  `dictionary-create-or-replace-ctas-file-op-marker-crash`, and
  `dictionary-create-or-replace-after-drop-crash` passed.
- The first hook CTest subset run hit an isolated
  `dictionary-ctas-file-op-marker-crash` `MYLITE_BUSY` open failure before the
  replacement-LIKE selector. The failed scratch directory was removed, the CTAS
  selector passed directly, and the same CTest subset rerun passed 9/9:
  `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-table-crash|create-or-replace-after-drop-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production direct selectors passed:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`,
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`,
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`, and
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-like-file-op-marker-crash`.
- `cmake --build --preset prod --target format-check` passed.
- `git diff --check` passed.
- No ownerless test processes or `/tmp/mylite-ownerless-*` directories remained
  after verification.

## Risks And Follow-Up

- Populated replacement CTAS requires separate live recovery because it must
  prove both copied metadata and copied rows.
- Parenthesized `(LIKE source)` syntax remains unclaimed until a focused
  classifier and test are added.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
