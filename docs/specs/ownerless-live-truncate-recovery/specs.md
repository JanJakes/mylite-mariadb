# Ownerless Live Truncate Recovery

## Problem Statement

Ownerless live recovery now covers CREATE-family native file creation,
replacement-copy boundaries, and a conservative single-pair same-schema
`RENAME TABLE`. `TRUNCATE TABLE` is the next bounded file-lifecycle class:
for InnoDB tables MariaDB can recreate the table with the same definition and
empty contents before MyLite reaches the `dictionary-before-finish` hook.

Existing hook coverage proves that a killed truncate writer leaves the native
file-operation checkpoint-needed marker and that no-live recovery preserves an
empty table. This slice upgrades the focused non-temporary
`TRUNCATE TABLE schema.table` path so another live ownerless opener can finish
the dead dictionary generation while a peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_truncate.cc`
  - `Sql_cmd_truncate_table::execute()` checks `DROP_ACL` and delegates to
    `truncate_table()`.
  - `Sql_cmd_truncate_table::lock_table()` takes an exclusive metadata lock,
    removes the table definition cache share when needed, and reports whether
    the handlerton can recreate the table.
  - `Sql_cmd_truncate_table::truncate_table()` calls
    `dd_recreate_table()` for engines with `HTON_CAN_RECREATE`, otherwise it
    uses the handler truncate path.
- `mariadb/sql/sql_table.cc`
  - Drop-style file lifecycle code uses DDL logging and handler file removal
    for related file-operation classes. This remains separate follow-up for
    `DROP TABLE`.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the durable native file-op
    checkpoint marker before `dictionary-before-finish` and records a
    recoverable dictionary kind only when native file-operation redo was seen.
  - Dead-owner cleanup normally keeps active transaction, lock, page-write, or
    redo state on the no-live path. This slice uses the durable recoverable
    dictionary marker as the gate before those stale records can be released
    for the dead owner.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for explicit non-temporary
  `TRUNCATE TABLE schema.table`.
- Use a conservative raw-token classifier that requires the `TABLE` keyword,
  an explicit `schema.table`, and no trailing clauses beyond `;`.
- Convert the focused truncate hook selector to prove live recovery, empty
  table state, post-recovery writes, live marker retention, no-live marker
  drain, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Unqualified `TRUNCATE table`.
- Partition-level truncate or `ALTER TABLE ... TRUNCATE PARTITION`.
- Foreign-key parent/child truncate variants.
- Temporary-table truncate.
- `DROP TABLE`, rebuild/copy-style `ALTER`, schema, view, trigger, partition,
  import/export, or metadata-only DDL live recovery.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state already stores recovery kind, owner id, and owner
generation. Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE`, accept
it in primitive validation, and include it in dead-owner cleanup's recovery
kind probe list.

`ownerless_dictionary_recovery_kind_for_statement()` adds a conservative raw
token classifier for exactly `TRUNCATE TABLE schema.table`. It rejects
unqualified, temporary, partition, and clause-bearing forms by shape. Broader
truncate classes therefore remain on the existing no-live path until focused
tests are added.

Live recovery only finishes the ownerless dictionary generation. The native
file-op checkpoint-needed marker remains durable while any peer is live and is
drained by the existing final no-live checkpoint path.

Unlike the earlier CREATE/RENAME live-recovery selectors, the truncate hook
window can still have stale per-owner transaction, lock, page-write, or redo
records that normal post-statement cleanup would have released after
`ownerless_finish_dictionary_ddl()`. Dead-owner cleanup therefore attempts
recoverable dictionary completion before deciding that those records require
no-live recovery. It relaxes cleanup only when the dictionary generation is
successfully recovered from the durable native file-op marker; otherwise the
existing busy-until-no-live policy remains unchanged.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after focused `TRUNCATE TABLE schema.table` can now
be recovered by a live ownerless opener. The table remains present and empty,
accepts post-recovery writes, and stays durable through ownerless/native
reopen.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native InnoDB truncate/recreate path as the storage authority and keeps the
MyLite native file-op checkpoint-needed marker durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-truncate-file-op-marker-crash`,
  `dictionary-truncate-crash`, `dictionary-rename-file-op-marker-crash`, and
  `dictionary-drop-file-op-marker-crash`.
- Run the focused hook CTest subset for the DDL file-op marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `truncated-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-truncate-file-op-marker-crash`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Truncate crash coverage no longer expects `MYLITE_BUSY` while a peer is live.
- A live ownerless opener recovers the dead truncate writer, observes the table
  present and empty, inserts a row, and verifies aggregates.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the
  marker and preserves truncate state through ownerless/native reopen and
  forced `.shm` rebuild.
- Drop, rebuild, schema, and broader truncate classes remain explicitly
  planned.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-truncate-file-op-marker-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-truncate-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-rename-file-op-marker-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-drop-file-op-marker-crash`
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|create-like-file-op-marker-crash|ctas-file-op-marker-crash|create-or-replace-table-crash|create-or-replace-after-drop-crash|create-or-replace-like-file-op-marker-crash|create-or-replace-ctas-file-op-marker-crash|rename-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test truncated-tablespace-replay`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-truncate-file-op-marker-crash`
- `cmake --build --preset prod --target format-check`
- `git diff --check`
- `find /tmp -maxdepth 1 -type d -name 'mylite-ownerless-*' -print`

## Risks And Follow-Up

- Foreign-key and partition truncate classes need separate compatibility and
  crash-recovery coverage.
- `DROP TABLE` is the next table-file lifecycle class because it removes the
  `.frm`/`.ibd` pair rather than recreating an empty table.
- Broader DDL/file-lifecycle recovery and external randomized stress remain
  open completion gates.
