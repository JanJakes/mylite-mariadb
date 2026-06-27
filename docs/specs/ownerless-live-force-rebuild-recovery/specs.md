# Ownerless Live Force Rebuild Recovery

## Problem Statement

Ownerless live recovery now covers CREATE-family file creation/replacement,
single-pair same-schema rename, focused truncate/recreate, and focused drop.
`ALTER TABLE ... FORCE, ALGORITHM=COPY` is the next bounded table
file-lifecycle class because MariaDB rebuilds the native table and secondary
indexes before MyLite reaches `dictionary-before-finish`.

Existing hook coverage proves that a killed force-rebuild writer leaves the
native file-operation checkpoint-needed marker and that no-live recovery keeps
the rebuilt table, index metadata, payload rows, and ownerless/native reopen
state. This slice upgrades the exact tested force-rebuild statement so another
live ownerless opener can finish the dead dictionary generation while a peer
remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `ALTER TABLE ... FORCE` sets `ALTER_RECREATE` for a same-engine table
    rebuild and marks `recreate_identical_table` when no other user-visible
    table options are changed.
  - COPY-algorithm ALTER runs through `copy_data_between_tables()`, writes
    rows to the rebuilt table, and completes the storage-engine alter-copy
    handoff before SQL-layer cleanup finishes.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  - InnoDB treats `ALTER TABLE ... FORCE`/`OPTIMIZE TABLE` as a rebuild class
    with no intended user-visible schema change.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the durable native file-op
    checkpoint marker before `dictionary-before-finish` and marks a
    recoverable dictionary kind only when native file-operation redo was seen.
  - Dead-owner cleanup consumes recoverable dictionary markers before treating
    stale owner transaction, lock, page-write, or redo records as
    no-live-only recovery state.

## Scope And Non-Goals

In scope:

- Add a distinct recovery kind for the focused force-rebuild statement:
  `ALTER TABLE schema.table FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`.
- Use a conservative raw-token classifier that requires the explicit
  `schema.table`, `FORCE`, `ALGORITHM=COPY`, and `LOCK=EXCLUSIVE` shape.
- Convert the focused force-rebuild hook selector to prove live recovery,
  rebuilt table/index/payload state, live marker retention, no-live marker
  drain, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Plain `ALTER TABLE schema.table FORCE` without explicit algorithm/lock is
  covered by the follow-up
  `docs/specs/ownerless-plain-force-rebuild-live-recovery/specs.md` slice.
- Row-format rebuilds, compressed key-block rebuilds, primary-key rebuilds,
  charset conversion, column rebuilds, foreign-key/check rebuilds, and other
  ALTER shapes.
- Rebuilds over partitioned tables or external tablespace/import-export paths.
- Clearing native file-op markers while peers remain live.
- External MariaDB/RQG stress.

## Design

The shared dictionary state stores recovery kind, owner id, and owner
generation. Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_FORCE_REBUILD`,
accept it in primitive validation, and include it in dead-owner cleanup's
recovery-kind probe list.

`ownerless_dictionary_recovery_kind_for_statement()` adds an exact raw-token
classifier for `ALTER TABLE schema.table FORCE, ALGORITHM=COPY,
LOCK=EXCLUSIVE`. It rejects unqualified tables, other ALTER subcommands,
row-format rebuilds, different algorithm/lock clauses, and additional clauses
by shape. Broader ALTER rebuild classes stay on the no-live path until focused
live tests are added.

Live recovery finishes only the ownerless dictionary generation. The rebuilt
table files, InnoDB table identity, secondary-index metadata, and rows remain
owned by MariaDB native storage. The native file-op checkpoint-needed marker
stays durable while any peer is live, and final no-live checkpoint drain
clears it after the held peer closes.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after the focused force-rebuild ALTER can now be
recovered by a live ownerless opener. The rebuilt table remains readable and
writable, secondary-index forced reads continue to work, and final state stays
durable through ownerless/native reopen.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The slice relies on MariaDB's
native table-copy rebuild as the storage authority and keeps the MyLite native
file-op checkpoint-needed marker durable until no-live drain.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The change adds one internal recovery-kind constant and focused
tests/docs.

## Test And Verification Plan

- Build hook targets for primitives and ownerless SQL.
- Run hook primitives.
- Run focused hook selectors:
  `dictionary-force-rebuild-file-op-marker-crash`,
  `dictionary-force-rebuild-crash`, `dictionary-row-format-file-op-marker-crash`,
  and `dictionary-drop-file-op-marker-crash`.
- Run the focused hook CTest subset for DDL file-op marker selectors.
- Build production primitives and ownerless SQL.
- Run production primitives, `native-file-op-marker-drain`,
  `dictionary-force-rebuild-file-op-marker-crash`, and
  `force-rebuild-tablespace-replay`.
- Run `format-check`, `tools/check-ci-production-builds`, `git diff --check`,
  and cached diff checks.

## Acceptance Criteria

- Force-rebuild crash coverage no longer expects `MYLITE_BUSY` while a peer is
  live.
- A live ownerless opener recovers the dead force-rebuild writer, observes the
  rebuilt table, verifies row payloads and secondary-index metadata/use, and
  verifies `.frm`/`.ibd` presence.
- The native file-op marker remains set after live recovery while the original
  peer is still live.
- After the final live peer closes, no-live ownerless recovery drains the
  marker and preserves rebuilt state through ownerless/native reopen and
  forced `.shm` rebuild.
- Row-format, compressed, primary-key, charset, column, constraint, and other
  ALTER rebuild classes remain planned; plain `ALTER TABLE schema.table FORCE`
  is covered by the follow-up plain-force slice.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- Hook selectors passed:
  `dictionary-force-rebuild-file-op-marker-crash`,
  `dictionary-force-rebuild-crash`, `dictionary-row-format-file-op-marker-crash`,
  `dictionary-drop-file-op-marker-crash`, and
  `dictionary-truncate-file-op-marker-crash`.
- Focused `ctest --preset ownerless-test-hooks` DDL marker subset passed
  16/16.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selectors passed: primitives, `native-file-op-marker-drain`,
  `dictionary-force-rebuild-file-op-marker-crash`, and
  `force-rebuild-tablespace-replay`.
- `cmake --build --preset prod --target format-check`,
  `tools/check-ci-production-builds`, and `git diff --check` passed.
- No `/tmp/mylite-ownerless-*` directories or ownerless test processes
  remained after verification.

## Risks And Follow-Up

- Rebuild ALTERs are a broad family. This slice only covers the exact forced
  rebuild shape already isolated by hook coverage.
- Row-format and compressed rebuilds have separate marker coverage and should
  get separate live-recovery slices because their metadata and page-layout
  assertions differ.
- Broader native redo/checkpoint reconciliation and external randomized stress
  remain open completion gates.
