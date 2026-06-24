# Ownerless Live Create Like Recovery

## Problem Statement

The first live-peer DDL recovery slice covers plain non-temporary InnoDB
`CREATE TABLE` killed after native file creation reaches MyLite dictionary
prefinish. The next adjacent `FILE_CREATE` class is `CREATE TABLE ... LIKE`:
MariaDB copies source-table metadata, creates an empty destination table and
file-per-table tablespace, and MyLite then reaches the same dictionary
prefinish crash boundary.

Existing hook coverage proves this boundary persists the native file-op marker
and that no-live recovery preserves the copied table, copied secondary-index
metadata, native `.frm`/`.ibd`, and later writes. This slice upgrades only that
LIKE boundary so a live peer can recover the dead dictionary owner without
waiting for all peers to close. CTAS and replacement-copy forms remain separate
because they add populated-row or old-target replacement semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `Sql_cmd_create_table_like::execute()` dispatches ordinary create, CTAS,
    and `CREATE TABLE ... LIKE`.
  - `mysql_create_like_table()` starts around line `5730`, opens the source
    table, prepares copied metadata, and calls `mysql_create_table_no_lock()`
    around line `5861` to create the destination.
  - `mysql_create_table_no_lock()` builds the destination path and delegates to
    `create_table_impl()` for the ordinary non-temporary destination create.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `create_table_info_t::create_table_def()` creates the destination InnoDB
    dictionary object and delegates durable non-temporary tables to
    `row_create_table_for_mysql()`.
- `mariadb/storage/innobase/fil/fil0fil.cc`
  - `fil_ibd_create()` logs native `FILE_CREATE` redo before destination file
    creation, which MyLite observes through the generic file-op redo hook.
- `packages/libmylite/src/database.cc`
  - The plain-create live recovery slice records a per-owner recoverable
    dictionary marker after the durable native file-op marker is written, then
    lets dead-owner cleanup finish the dictionary generation only after native
    transaction, lock, page-write, and redo owner state are idle.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `dictionary-create-like-file-op-marker-crash` already kills the writer at
    `dictionary-before-finish`, asserts the marker is durable while another
    peer is live, and then currently expects live-peer cleanup to return busy.

## Scope And Non-Goals

In scope:

- Add a separate
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE_LIKE` recovery kind.
- Classify non-temporary `CREATE TABLE ... LIKE` as recoverable only after the
  durable native file-op marker is written.
- Let dead-owner cleanup finish the matching LIKE dictionary generation while
  peers remain live, reusing the existing native-state idle gates.
- Convert focused LIKE hook coverage to prove live recovery, copied metadata,
  copied secondary index usability, marker retention while a peer is live, and
  no-live marker drain.

Out of scope:

- CTAS live recovery.
- `CREATE OR REPLACE TABLE ... LIKE` replacement-copy recovery.
- Plain create recovery, already covered by
  `docs/specs/ownerless-live-create-tablespace-recovery/specs.md`.
- Rename, truncate, drop, rebuild, schema, view, trigger, foreign-key multi-DDL,
  partition/import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers with live peers.
- External MariaDB/RQG stress.

## Design

Extend the dictionary recovery kind namespace with a LIKE-specific value. The
shared dictionary state already stores recovery kind, owner id, and owner
generation, so no `.shm` segment-size change is needed.

`ownerless_dictionary_recovery_kind_for_statement()` will continue to classify
plain create separately, then classify `CREATE TABLE ... LIKE` when the
identifier stream starts with `CREATE`, includes `TABLE`, includes `LIKE`,
does not include `TEMPORARY`, `OR REPLACE`, or `SELECT`, and has no raw
definition-list `(` before the `LIKE` token. That keeps `CHECK (col LIKE ...)`
and other expression-level `LIKE` usage out of this live-recovery class. The
existing ownerless SQL policy and MariaDB parser remain responsible for
rejecting unsupported storage engines and malformed statements.

Dead-owner cleanup will attempt recovery for the stored kind rather than only
the plain-create kind. It still requires the global native file-op marker and
the existing idle native-state proofs. The marker is not cleared by live
recovery; final no-live checkpoint/reclaim keeps that responsibility.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after native `CREATE TABLE ... LIKE` destination
creation can now be recovered by a live ownerless opener. Broader DDL
file-lifecycle recovery remains partial.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
created destination `.frm` and `.ibd` files as the final storage authority and
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
  `dictionary-create-like-file-op-marker-crash`,
  `dictionary-create-table-live-recovery`, and adjacent still-no-live CTAS,
  rename, truncate, and drop marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-create-like-file-op-marker-crash`.
- Run `format-check-prod`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The LIKE crash selector no longer expects `MYLITE_BUSY` while the peer is
  live.
- A live ownerless opener recovers the dead LIKE creator, observes copied table
  and secondary-index metadata, inserts rows, and verifies forced-index reads.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the
  marker and preserves copied table state through ownerless/native reopen and
  forced `.shm` rebuild.
- CTAS and replacement-copy marker crash selectors still require no-live
  recovery.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-like-file-op-marker-crash`
  passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
  passed 7/7.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test created-tablespace-replay`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-like-file-op-marker-crash`
  passed in the production hook-disabled binary.

## Risks And Follow-Up

- The classifier stays conservative; unrecognized valid LIKE spellings,
  including any table-copy spelling that requires a parenthesized form, keep the
  older busy-until-no-live behavior.
- CTAS may require additional row-population and transaction-state reasoning
  before live recovery can be safely enabled.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
