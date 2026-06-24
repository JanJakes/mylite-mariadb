# Ownerless Live Create Or Replace CTAS Recovery

## Problem Statement

Plain create, table-copy create, focused CTAS, ordinary replacement, and
replacement-copy LIKE prefinish crash boundaries can be recovered by a live
ownerless peer after the durable native file-operation marker is published.
Replacement-copy `CREATE OR REPLACE TABLE ... AS SELECT` is the populated-row
counterpart: MariaDB removes the existing target table, creates a replacement
from the SELECT result, populates the new table, then reaches MyLite's
`dictionary-before-finish` hook.

Existing hook coverage proved the populated replacement after no-live recovery.
This slice upgrades only the conservative no-definition-list
`CREATE OR REPLACE TABLE ... AS SELECT` form so a live ownerless opener can
finish the dead dictionary generation while another peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `Sql_cmd_create_table_like::execute()` takes the CTAS branch when the
    parser attached a SELECT list or table-value constructor.
  - The CTAS branch opens and locks source tables, unlinks the target from the
    SELECT namespace, constructs `select_create`, and calls `handle_select()`;
    `select_create` performs target creation and row insertion.
  - `create_info.or_replace()` keeps same-name replacement semantics while
    guarding against selecting from the target table.
- `mariadb/sql/sql_yacc.yy`
  - `opt_create_select` attaches the SELECT branch after table options. This
    slice covers the no-definition-list form already used by ownerless CTAS
    crash tests, not `CREATE ... (columns) SELECT ...`.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the native file-op checkpoint
    marker before `dictionary-before-finish` and records a recoverable
    dictionary marker only when InnoDB observed native file-operation redo.
  - Dead-owner dictionary recovery requires the durable file-op marker plus the
    existing idle native-state gates before it can finish an odd dictionary
    generation while peers remain live.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for non-temporary no-definition-list
  `CREATE OR REPLACE TABLE ... AS SELECT`.
- Classify the conservative replacement CTAS form and keep LIKE, temporary
  tables, views, routines, schemas, and definition-list CTAS out.
- Convert the focused replacement-CTAS hook selector to prove live recovery,
  copied row visibility, old-column/index absence, post-recovery writes, live
  marker retention, no-live marker drain, ownerless/native reopen, and forced
  `.shm` rebuild.

Out of scope:

- Definition-list CTAS replacement syntax.
- Parenthesized LIKE-copy replacement syntax.
- The `create-or-replace-after-drop` absence boundary.
- Rename, truncate, drop, rebuild, schema, view, trigger, foreign-key multi-DDL,
  partition/import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state already stores recovery kind, owner id, and owner
generation, so no `.shm` layout change is required. Add
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE_SELECT`, accept it
in primitive validation, and include it in dead-owner cleanup's recovery-kind
probe list.

`ownerless_dictionary_recovery_kind_for_statement()` keeps ordinary replacement
and replacement-LIKE classifiers ahead of the new CTAS classifier. The new
classifier accepts identifier streams beginning with `CREATE OR REPLACE TABLE`,
rejects `TEMPORARY` and `LIKE`, rejects non-table CREATE object classes before
the `TABLE` keyword, then requires `SELECT` after `TABLE` with no raw `(`
before it. This mirrors the existing conservative CTAS classifier and avoids
claiming definition-list CTAS until there is focused coverage.

Live recovery only finishes the dictionary generation. The native file-op
marker remains set while any peer is live and is drained by the existing final
no-live checkpoint path.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after `CREATE OR REPLACE TABLE ... AS SELECT` can
now be recovered by a live ownerless opener. The replacement table uses
MariaDB's native copied CTAS metadata and populated rows, accepts post-recovery
writes, and remains durable through ownerless/native reopen.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native replacement `.frm` and `.ibd` files plus populated InnoDB rows as the
storage authority and keeps the MyLite native file-op checkpoint-needed marker
durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-create-or-replace-ctas-file-op-marker-crash`,
  `dictionary-create-or-replace-like-file-op-marker-crash`,
  `dictionary-create-or-replace-table-crash`, and adjacent after-drop,
  create-table, create-like, CTAS, rename, truncate, and drop marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-create-or-replace-ctas-file-op-marker-crash`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Replacement-CTAS crash coverage no longer expects `MYLITE_BUSY` while a peer
  is live.
- A live ownerless opener recovers the dead replacement-CTAS writer, observes
  the populated target, verifies old-column/index absence and replacement
  column metadata, inserts a row, and verifies final aggregates.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the marker
  and preserves replacement state through ownerless/native reopen and forced
  `.shm` rebuild.
- After-drop replacement recovery remains a no-live guard.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed after removing the now-unused marker-specific live-peer helper.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-ctas-file-op-marker-crash`
  passed.
- Adjacent direct hook selectors
  `dictionary-create-or-replace-ctas-crash`,
  `dictionary-create-or-replace-like-file-op-marker-crash`, and
  `dictionary-create-or-replace-after-drop-crash` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-table-crash|create-or-replace-after-drop-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
  passed 9/9.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production direct selectors passed:
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`,
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`,
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`, and
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-ctas-file-op-marker-crash`.
- `cmake --build --preset prod --target format-check` passed.
- `git diff --check` passed.
- No ownerless test processes or `/tmp/mylite-ownerless-*` directories remained
  after verification.

## Risks And Follow-Up

- Definition-list CTAS remains unclaimed until a focused classifier and test are
  added.
- The after-drop absence boundary must remain no-live until absence recovery is
  designed separately.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
