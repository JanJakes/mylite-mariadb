# Ownerless Live Drop Recovery

## Problem Statement

Focused live recovery now covers CREATE-family file creation/replacement,
single-pair same-schema rename, and focused truncate/recreate. `DROP TABLE` is
the next table file-lifecycle class because MariaDB removes the durable
definition and file-per-table tablespace before MyLite reaches
`dictionary-before-finish`.

Existing hook coverage proves that a killed drop writer leaves the native
file-operation checkpoint-needed marker and that no-live recovery keeps the
table absent. This slice upgrades the focused single-table
`DROP TABLE schema.table` path so another live ownerless opener can finish the
dead dictionary generation while a peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table()` validates drop targets, locks table names, removes
    table statistics, and delegates to `mysql_rm_table_no_locks()`.
  - `mysql_rm_table_no_locks()` acquires table-share metadata, initializes DDL
    drop logging, removes the table from the table definition cache, calls
    `ddl_log_drop_table()`, and invokes `ha_delete_table()` for the native
    handler/file removal.
  - `ddl_log_drop_after_delete_table` is reached after handler deletion and
    before SQL-layer cleanup completes.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the durable native file-op
    checkpoint marker before `dictionary-before-finish` and records a
    recoverable dictionary kind only when native file-operation redo was seen.
  - Dead-owner cleanup first consumes a recoverable dictionary marker before
    allowing stale per-owner transaction, lock, page-write, or redo records to
    be released for the dead owner.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for explicit non-temporary
  `DROP TABLE schema.table`.
- Use a conservative raw-token classifier that requires an explicit
  `schema.table` and no trailing clauses beyond `;`.
- Convert the focused drop hook selector to prove live recovery, table absence,
  `.frm`/`.ibd` absence, live marker retention, no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- `DROP TABLE IF EXISTS`.
- Multi-table drop.
- Cross-schema same-statement drop.
- Temporary-table drop.
- `DROP DATABASE`, view/trigger/routine drop, partition/import/export, or
  metadata-only DDL live recovery.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state stores recovery kind, owner id, and owner
generation. Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE`, accept it
in primitive validation, and include it in dead-owner cleanup's recovery-kind
probe list.

`ownerless_dictionary_recovery_kind_for_statement()` adds a conservative raw
token classifier for exactly `DROP TABLE schema.table`. It rejects `IF EXISTS`,
multi-drop, temporary, unqualified, and clause-bearing forms by shape. Broader
drop classes stay on the existing no-live path until focused live tests are
added.

Live recovery finishes only the ownerless dictionary generation. The dropped
table files remain absent from MariaDB native storage, and the native file-op
checkpoint-needed marker remains durable while any peer is live. The existing
final no-live checkpoint path drains the marker after the held peer closes.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after focused `DROP TABLE schema.table` can now be
recovered by a live ownerless opener. The table remains absent, the `.frm` and
`.ibd` files remain absent, and the final state stays durable through
ownerless/native reopen.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native InnoDB file removal as the storage authority and keeps the MyLite native
file-op checkpoint-needed marker durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-drop-file-op-marker-crash`,
  `dictionary-drop-crash`, `dictionary-truncate-file-op-marker-crash`, and
  `dictionary-rename-file-op-marker-crash`.
- Run the focused hook CTest subset for the DDL file-op marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `native-file-op-marker-drain`, and hook-disabled
  `dictionary-drop-file-op-marker-crash`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Drop crash coverage no longer expects `MYLITE_BUSY` while a peer is live.
- A live ownerless opener recovers the dead drop writer, observes the table
  absent, verifies query failure for the dropped table, and verifies `.frm` and
  `.ibd` absence.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the
  marker and preserves drop state through ownerless/native reopen and forced
  `.shm` rebuild.
- Multi-table, cross-schema, schema-drop, and other object-drop classes remain
  planned.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- Hook selectors passed:
  `dictionary-drop-file-op-marker-crash`, `dictionary-drop-crash`,
  `dictionary-truncate-file-op-marker-crash`,
  `dictionary-rename-file-op-marker-crash`, and
  `dictionary-cross-schema-rename-crash`.
- Focused `ctest --preset ownerless-test-hooks` DDL marker subset passed
  9/9.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selectors passed: primitives, `native-file-op-marker-drain`,
  `dictionary-drop-file-op-marker-crash`, and `dropped-tablespace-replay`.
- `cmake --build --preset prod --target format-check`,
  `tools/check-ci-production-builds`, and `git diff --check` passed.
- No `/tmp/mylite-ownerless-*` directories or ownerless test processes
  remained after verification.

## Risks And Follow-Up

- Multi-table and cross-schema drops need separate live recovery because one
  statement can remove multiple schema/table assets.
- `DROP DATABASE` remains separate because it removes a whole schema directory
  and multiple objects.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
