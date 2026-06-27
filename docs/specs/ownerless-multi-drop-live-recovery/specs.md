# Ownerless Multi Drop Live Recovery

## Problem Statement

Focused ownerless drop recovery covers single-table explicit and implicit
`DROP TABLE` spellings. Same-statement multi-table drops previously remained
no-live-only even though MariaDB removes each native `.frm`/`.ibd` pair before
MyLite reaches `dictionary-before-finish`.

MyLite should recover bounded multi-table `DROP TABLE` lists while another
ownerless peer remains live, preserving the final absent table/file state and
retaining the native file-operation checkpoint-needed marker until the final
peer exits.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The `drop` grammar parses `DROP opt_temporary table_or_tables
    opt_if_exists ... table_list ...` and sets `SQLCOM_DROP_TABLE`.
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_DROP_TABLE` dispatches to `mysql_rm_table()` with the parsed
    table list and `if_exists`/temporary flags.
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table()` locks every table name in the `TABLE_LIST`, removes
    statistics, and delegates to `mysql_rm_table_no_locks()`.
  - `mysql_rm_table_no_locks()` iterates `tables`, acquires table-share
    metadata, writes DDL-log drop records, calls `ha_delete_table()` for each
    native table, deletes `.frm`, and continues through the list.
- `packages/libmylite/src/database.cc`
  - Before this slice, the ownerless `DROP TABLE` recovery classifier accepted
    exactly one one- or two-part table identifier and rejected
    comma-separated lists.
  - `ownerless_finish_dictionary_ddl()` records a recoverable dictionary kind
    before `dictionary-before-finish` when native file-operation redo has
    produced the checkpoint-needed marker.
- `docs/specs/ownerless-implicit-drop-recovery/specs.md`
  - Prior exploration documented that an attempted same-statement multi-drop
    selector hit a native InnoDB purge assertion before the MyLite hook. This
    slice must verify the current hook path before claiming coverage.

## Design

Broaden `ownerless_drop_table_recovery_statement()` from one table identifier
to a comma-separated list of one- or two-part table identifiers:

- `DROP TABLE app.table_a, app.table_b`
- `DROP TABLE app.table_a, other_schema.table_b`

The classifier continues to reject:

- `DROP TEMPORARY TABLE ...`
- `DROP TABLE IF EXISTS ...`
- trailing lock-wait/restrict clauses,
- empty lists, dangling commas, and non-table object drops.

The implementation reuses
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE`; no new persistent recovery
kind is needed because the native file-operation checkpoint marker plus that
drop recovery kind already identify the file-lifecycle lane.

Add two unsafe-hook selectors:

- `dictionary-multi-drop-crash`
- `dictionary-cross-schema-multi-drop-crash`

Both selectors create two InnoDB file-per-table tables, kill a writer at
`dictionary-before-finish` after one `DROP TABLE` list completes natively, open
a new ownerless handle while a peer remains live, verify both tables and both
native file pairs are absent, verify the native file-operation marker remains
set while the peer is live, release the peer, and verify final marker drain plus
ownerless/native reopen before and after forced `.shm` rebuild.

This follow-up registers both selectors as standalone `ownerless-test-hooks`
CTest jobs so the same-schema and cross-schema multi-drop file-lifecycle crash
gates have visible CI timing and failure attribution instead of being visible
only through direct selector runs or broad ownerless SQL shards.

## Scope

In scope:

- Same-schema two-table `DROP TABLE` list live recovery.
- Cross-schema two-table `DROP TABLE` list live recovery.
- Native `.frm`/`.ibd` absence for every dropped table.
- Live marker retention and final no-live marker drain.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `DROP TABLE IF EXISTS` no-op lists.
- `DROP TEMPORARY TABLE`.
- Schema drop, partitioned tables, table-directory options, DISCARD/IMPORT
  tablespace, and view/trigger/routine drops.
- Crash injection between individual table drops inside MariaDB's internal
  loop.
- SQL-level table-lock fault injection.
- External MariaDB/RQG stress.

## Compatibility Impact

No successful SQL behavior changes. The slice strengthens ownerless recovery
for completed MariaDB multi-table drop lists when a writer dies at MyLite's
dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes. The slice relies on MariaDB native file removal
for each table and keeps MyLite's native file-operation checkpoint-needed
marker durable until no-live native checkpoint proof drains it.

## Native Storage Impact

No storage format changes. InnoDB file-per-table `.ibd` removal remains the
storage authority. The tests verify dropped tables are not resurrected through
ownerless/native reopen or forced `.shm` rebuild.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-multi-drop-crash`
  - `dictionary-cross-schema-multi-drop-crash`
- Run focused CTests:
  - `libmylite.ownerless-dictionary-multi-drop-crash`
  - `libmylite.ownerless-dictionary-cross-schema-multi-drop-crash`
- Run adjacent drop selectors:
  - `dictionary-drop-file-op-marker-crash`
  - `dictionary-drop-crash`
  - `dictionary-implicit-drop-crash`
- Run no-live replay selectors:
  - `multi-drop-tablespace-replay`
  - `cross-schema-multi-drop-tablespace-replay`
- Build production ownerless SQL target and run representative drop/DDL
  selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- The focused multi-drop writers reach `dictionary-before-finish`; if the
  native purge assertion still reproduces before the hook, the slice is blocked
  and must not claim coverage.
- A live ownerless opener recovers the dead dictionary generation while another
  peer remains open.
- Every dropped table is absent from `INFORMATION_SCHEMA.TABLES`.
- Every dropped table rejects direct reads.
- Every native `.frm` and `.ibd` file pair is absent.
- The native file-operation checkpoint-needed marker remains set while the peer
  is live and drains after final no-live recovery.
- Ownerless and ordinary native reopen observe the same absent-table state
  before and after forced `.shm` rebuild.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  dictionary-multi-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  dictionary-cross-schema-multi-drop-crash`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-dictionary-(multi-drop|cross-schema-multi-drop)-crash$'
  --output-on-failure`
- `ctest --preset ownerless-test-hooks -R
  '^(libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-drop-file-op-marker-crash|libmylite\.ownerless-dictionary-implicit-drop-crash)$'
  --output-on-failure`
- Direct hook selectors:
  - `dictionary-drop-crash`
  - `multi-drop-tablespace-replay`
  - `cross-schema-multi-drop-tablespace-replay`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2`
- Production selectors:
  - `ddl-broader`
  - `native-file-op-marker-drain`
  - `multi-drop-tablespace-replay`
  - `cross-schema-multi-drop-tablespace-replay`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress-statement-lock-retry$'
  --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `git diff --check`

The first ownerless-stress DDL run with the previous default
`30` second statement-lock wait exhausted the harness retry deadline with
pre-execution `MYLITE_BUSY` contention before any multi-DROP-specific failure.
The registered DDL stress CTest now sets
`MYLITE_OWNERLESS_DDL_STRESS_LOCK_WAIT_TIMEOUT=1`, matching the existing
bounded retry model; the same eight-round CTest passed in `100.27 sec`, and
the focused immediate-retry selector passed in `11.77 sec`.

## Risks And Follow-Up

- This does not prove crash recovery between individual drops in MariaDB's
  internal loop; it proves the completed native multi-drop boundary before
  ownerless dictionary finish.
- `DROP TABLE IF EXISTS` and temporary-table drop lists need separate policy.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
