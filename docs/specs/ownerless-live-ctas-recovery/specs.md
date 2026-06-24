# Ownerless Live CTAS Recovery

## Problem Statement

Plain non-temporary InnoDB `CREATE TABLE` and non-temporary
`CREATE TABLE ... LIKE` can now be recovered by a live ownerless peer when the
writer dies at MyLite's `dictionary-before-finish` hook after the durable native
file-operation marker is published. The adjacent remaining create-table class is
`CREATE TABLE ... SELECT` (CTAS): MariaDB creates the destination table, inserts
selected rows, commits the statement, and then MyLite reaches the same
dictionary prefinish boundary.

Existing hook coverage proves the CTAS boundary leaves a durable native
file-op marker and that no-live recovery preserves the created `.frm`/`.ibd`,
destination columns, copied rows, post-recovery writes, ownerless/native reopen,
and forced `.shm` rebuild. This slice upgrades only that post-commit CTAS
prefinish boundary so a live peer can finish the dead dictionary generation
without waiting for every peer to close.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `Sql_cmd_create_table_like::execute()` dispatches CTAS when the select list
    or table-value constructor is present, opens and locks source tables, then
    constructs a `select_create` result receiver.
  - The non-CTAS path dispatches ordinary creates and `CREATE TABLE ... LIKE`
    separately, so CTAS can be classified with a separate MyLite recovery kind.
- `mariadb/sql/sql_insert.cc`
  - `select_create::create_table_from_items()` appends selected item metadata
    to the create list and calls `mysql_create_table_no_lock()` to create and
    open the destination table.
  - `select_create::send_eof()` commits the populated non-temporary CTAS
    statement through `trans_commit_stmt()` and `trans_commit_implicit()` before
    logging DDL completion and returning success.
- `mariadb/storage/innobase/fil/fil0fil.cc`
  - `fil_ibd_create()` logs the destination `FILE_CREATE` redo that MyLite uses
    for the generic native file-operation marker.
- `packages/libmylite/src/database.cc`
  - Dead-owner dictionary recovery already requires the durable native file-op
    marker, no active native transaction/lock/page-write/redo owner state for
    the dead process, and a matching recoverable dictionary marker.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `dictionary-ctas-file-op-marker-crash` already kills the writer at
    `dictionary-before-finish`, asserts marker durability while another peer is
    live, verifies live-peer cleanup currently returns busy, and then verifies
    the final populated CTAS table after no-live recovery.

## Scope And Non-Goals

In scope:

- Add a separate `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_SELECT`
  recovery kind.
- Classify non-temporary no-definition-list CTAS spellings that start with
  `CREATE TABLE`, include `SELECT`, and do not include `TEMPORARY`,
  `OR REPLACE`, or `LIKE`.
- Let dead-owner cleanup finish the matching CTAS dictionary generation while
  peers remain live, reusing the existing native-state idle gates.
- Convert focused CTAS hook coverage to prove live recovery, populated-row
  visibility, live marker retention, no-live marker drain, ownerless/native
  reopen, and forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE TABLE ... AS SELECT` replacement-copy recovery.
- `CREATE TABLE ... LIKE`, already covered by
  `docs/specs/ownerless-live-create-like-recovery/specs.md`.
- Plain create recovery, already covered by
  `docs/specs/ownerless-live-create-tablespace-recovery/specs.md`.
- DML replay expansion for later post-create writes beyond the focused CTAS
  final-state checks already in the selector.
- Rename, truncate, drop, rebuild, schema, view, trigger, foreign-key multi-DDL,
  partition/import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers while live peers remain open.
- External MariaDB/RQG stress.

## Design

Extend the dictionary recovery kind namespace with a CTAS-specific value. The
shared dictionary state already stores recovery kind, owner id, and owner
generation, so no `.shm` segment-size change is needed.

`ownerless_dictionary_recovery_kind_for_statement()` will continue to classify
plain create and `CREATE TABLE ... LIKE` first, then classify CTAS when the
identifier stream starts with `CREATE`, contains `TABLE`, contains `SELECT`,
excludes `TEMPORARY`, `OR REPLACE`, and `LIKE`, and has no raw
definition-list `(` before the `SELECT` token. CTAS with an explicit
create-definition list stays on the older no-live path until that shape has its
own evidence. The existing ownerless SQL policy and MariaDB parser remain
responsible for malformed statements and unsupported storage options.

Dead-owner cleanup will attempt recovery for the stored kind. It still requires
the global native file-op marker and the existing idle native-state proofs. The
marker is not cleared by live recovery; final no-live checkpoint/reclaim keeps
that responsibility.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after non-temporary CTAS destination creation and row
population can now be recovered by a live ownerless opener. Broader DDL
file-lifecycle recovery remains partial.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
committed destination `.frm` and `.ibd` files and populated rows as the final
storage authority and keeps MyLite's native file-op checkpoint-needed marker
durable until the existing no-live checkpoint path drains it.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-ctas-file-op-marker-crash`,
  `dictionary-create-table-live-recovery`,
  `dictionary-create-like-file-op-marker-crash`, and adjacent still-no-live
  rename, truncate, drop, and replacement-copy marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-ctas-file-op-marker-crash`.
- Run `format-check-prod`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The CTAS crash selector no longer expects `MYLITE_BUSY` while the peer is
  live.
- A live ownerless opener recovers the dead CTAS creator, observes the created
  table and columns, sees the populated CTAS rows, inserts additional rows, and
  verifies aggregate state.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the marker
  and preserves CTAS state through ownerless/native reopen and forced `.shm`
  rebuild.
- Replacement-copy CTAS marker crash selectors still require no-live recovery.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-ctas-file-op-marker-crash`
  passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
  passed 9/9.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-ctas-file-op-marker-crash`
  passed in the production hook-disabled binary.

## Risks And Follow-Up

- The classifier stays conservative; unrecognized CTAS spellings, including
  explicit create-definition-list CTAS, keep the older busy-until-no-live
  behavior.
- Replacement-copy CTAS requires old-target lifecycle reasoning before live
  recovery can be safely enabled.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
