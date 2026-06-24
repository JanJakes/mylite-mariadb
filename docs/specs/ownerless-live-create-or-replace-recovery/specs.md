# Ownerless Live Create Or Replace Recovery

## Problem Statement

Plain create, `CREATE TABLE ... LIKE`, and focused no-definition-list CTAS
prefinish crash boundaries can now be recovered by a live ownerless peer after
the durable native file-operation marker is published. The next higher-risk DDL
class is ordinary `CREATE OR REPLACE TABLE`: MariaDB removes or replaces an
existing target table, creates a fresh replacement table, and MyLite then
reaches `dictionary-before-finish`.

Existing hook coverage proves no-live recovery preserves the replacement table
shape, removes old columns and indexes, keeps the new replacement rowset empty,
supports post-recovery writes, and survives ownerless/native reopen plus forced
`.shm` rebuild. This slice upgrades only the ordinary non-copy replacement
boundary so a live peer can finish the dead dictionary generation. Replacement
copy forms and the earlier after-drop absence boundary remain separate because
they have distinct old-target and source-table semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `Sql_cmd_create_table_like::execute()` copies `HA_CREATE_INFO` and
    `Alter_info`, resolves storage engine state, and dispatches ordinary create
    through `mysql_create_table()` when the statement is not CTAS or
    `CREATE TABLE ... LIKE`.
  - `mysql_create_table()` calls `mysql_create_table_no_lock()`, which delegates
    to `create_table_impl()` for the native replacement/create sequence.
  - `quick_rm_table()` is the native table-removal helper used across failed
    replacement cleanup and table lifecycle paths.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the native file-op marker before
    the prefinish hook only when InnoDB reports native file-operation redo, and
    only then records a recoverable per-owner dictionary marker.
  - Dead-owner dictionary recovery already requires the durable native file-op
    marker plus idle native transaction, lock, page-write, redo, and latch state
    for the dead process.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `dictionary-create-or-replace-table-crash` already kills the writer at
    `dictionary-before-finish` while a peer is live, expects live-peer cleanup
    to remain busy through the generic helper, then verifies the replacement
    table after no-live recovery.

## Scope And Non-Goals

In scope:

- Add a separate
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_TABLE` recovery kind.
- Classify non-temporary ordinary `CREATE OR REPLACE TABLE` statements that do
  not include `LIKE`, `SELECT`, or replacement-copy source semantics.
- Assert the ordinary replacement boundary has a durable native file-op marker
  before live recovery is allowed.
- Let dead-owner cleanup finish the matching replacement dictionary generation
  while peers remain live, reusing the existing native-state idle gates.
- Convert focused ordinary replacement hook coverage to prove live recovery,
  old-column/index absence, new-column/index presence, post-recovery writes,
  live marker retention, no-live marker drain, ownerless/native reopen, and
  forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE TABLE ... LIKE` replacement-copy recovery.
- `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy recovery.
- The `create-or-replace-after-drop` absence boundary.
- Rename, truncate, drop, rebuild, schema, view, trigger, foreign-key multi-DDL,
  partition/import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers while live peers remain open.
- External MariaDB/RQG stress.

## Design

Extend the dictionary recovery kind namespace with an ordinary-replacement
value. The shared dictionary state already stores recovery kind, owner id, and
owner generation, so no `.shm` segment-size change is needed.

`ownerless_dictionary_recovery_kind_for_statement()` will classify ordinary
replacement after the already-covered plain create, LIKE, and CTAS classes. The
classifier accepts identifier streams beginning with `CREATE OR REPLACE TABLE`
and rejects `TEMPORARY`, `LIKE`, and `SELECT`, keeping replacement-copy and
temporary-table forms out of this slice.

Dead-owner cleanup will attempt recovery for the stored kind. It still requires
the global native file-op marker and the existing idle native-state proofs. The
marker is not cleared by live recovery; final no-live checkpoint/reclaim keeps
that responsibility.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after ordinary `CREATE OR REPLACE TABLE` replacement
can now be recovered by a live ownerless opener. Broader DDL file-lifecycle
recovery remains partial.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native replacement `.frm` and `.ibd` state as the final storage authority and
keeps MyLite's native file-op checkpoint-needed marker durable until the
existing no-live checkpoint path drains it.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-create-or-replace-table-crash`,
  `dictionary-create-table-live-recovery`,
  `dictionary-create-like-file-op-marker-crash`,
  `dictionary-ctas-file-op-marker-crash`, and adjacent still-no-live
  replacement-copy, rename, truncate, and drop marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-create-or-replace-table-crash`.
- Run `format-check-prod`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The ordinary replacement crash selector no longer expects `MYLITE_BUSY` while
  the peer is live.
- A live ownerless opener recovers the dead replacement writer, observes the
  replacement table, verifies old-column/index absence and new-column/index
  presence, inserts rows, and verifies forced-index reads.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the marker
  and preserves replacement state through ownerless/native reopen and forced
  `.shm` rebuild.
- Replacement-copy and after-drop replacement selectors still require no-live
  recovery.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-table-crash`
  passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-table-crash|create-or-replace-after-drop-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
  passed 9/9 for the CTest-exposed selectors.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-after-drop-crash`
  passed.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-table-crash`
  passed in the production hook-disabled binary.

## Risks And Follow-Up

- Replacement-copy LIKE recovery is covered by
  `ownerless-live-create-or-replace-like-recovery`; populated CTAS replacement
  still requires additional source-table and row-population reasoning before
  live recovery can be safely enabled.
- The after-drop absence boundary must remain no-live until absence recovery is
  designed separately.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
