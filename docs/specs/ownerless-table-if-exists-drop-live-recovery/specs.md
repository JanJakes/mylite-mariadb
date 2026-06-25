# Ownerless Table If Exists Drop Live Recovery

## Problem Statement

Ownerless table idempotent DDL crash coverage already proves
`DROP TABLE IF EXISTS` no-op recovery after the final live peer exits. It does
not let another ownerless opener recover the dead dictionary generation while a
peer remains live, because the recovery classifier intentionally rejects
`IF EXISTS`.

MyLite should recover bounded single-table and multi-table
`DROP TABLE IF EXISTS` crashes at `dictionary-before-finish` while another
ownerless peer remains live, covering both outcomes MariaDB can produce:

- missing table no-op, with no native file-operation marker, and
- existing table removal, with the native file-operation checkpoint-needed
  marker preserved until final no-live drain.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `DROP opt_temporary table_or_tables opt_if_exists ... table_list ...`
    parses `DROP TABLE IF EXISTS`.
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table()` locks every table in the parsed list and delegates to
    `mysql_rm_table_no_locks()`.
  - `mysql_rm_table_no_locks()` continues after missing objects when
    `if_exists` is set, accumulating note diagnostics instead of failing the
    statement.
  - For existing non-temporary tables, the same loop removes native handler
    files and `.frm` metadata.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` marks a dictionary DDL recoverable
    when either a native file-operation marker was written or the recovery kind
    is metadata-only.
  - Dead-owner cleanup first tries file-operation recovery kinds only when the
    native file-operation checkpoint-needed marker is set, then tries
    metadata-only recovery kinds without that marker.

## Design

Add a distinct recovery kind:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE_IF_EXISTS`

Classify bounded `DROP TABLE IF EXISTS <identifier>[, <identifier> ...]`
statements into that recovery kind. Keep `DROP TEMPORARY TABLE IF EXISTS` out
of scope because temporary table state is connection-local and already
excluded from ownerless native file-lifecycle claims.

Register the new kind in both recovery lanes:

- file-operation recovery list, so existing-table `DROP TABLE IF EXISTS`
  recovers under the native file-operation marker path,
- metadata-only recovery list, so missing-table no-op `DROP TABLE IF EXISTS`
  recovers without a native marker.

Add one new unsafe-hook selector for existing-table removal and promote the
existing missing-table idempotent drop selector from no-live-only recovery to
held-live-peer recovery.

Extend the focused evidence with multi-table list selectors for:

- all-missing table names, proving the metadata-only marker-clear lane for a
  list, and
- two existing table names, proving marker retention and drain for multiple
  native table-file removals.

## Scope

In scope:

- Missing single-table `DROP TABLE IF EXISTS` live recovery.
- Existing single-table `DROP TABLE IF EXISTS` live recovery.
- Missing multi-table `DROP TABLE IF EXISTS` list live recovery.
- Existing multi-table `DROP TABLE IF EXISTS` list live recovery.
- Native `.frm`/`.ibd` preservation for the missing-table no-op case.
- Native `.frm`/`.ibd` removal plus marker retention/drain for the existing
  table-removal case.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Temporary table drops.
- Partitioned tables.
- Crash injection between individual tables in MariaDB's internal drop loop.
- SQL-level table-lock fault injection.
- External MariaDB/RQG stress.

## Compatibility Impact

No successful SQL behavior changes. The slice strengthens ownerless recovery
for MariaDB-compatible `DROP TABLE IF EXISTS` outcomes when a writer dies after
native execution and before ownerless dictionary finish.

## Directory And Lifecycle Impact

No directory layout changes. Missing-table no-op recovery must keep the real
table files untouched and the native file-operation marker clear. Existing
table removal must keep the native file-operation marker durable while a peer
remains live and drain it only after final no-live recovery.

## Native Storage Impact

No storage format changes. InnoDB file-per-table `.ibd` removal remains the
storage authority for existing-table drops, and no native table files are
created for missing-table no-ops.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-table-idempotent-drop-crash`
  - `dictionary-table-idempotent-existing-drop-crash`
  - `dictionary-table-idempotent-multi-drop-crash`
  - `dictionary-table-idempotent-multi-existing-drop-crash`
- Run adjacent table/drop selectors:
  - `dictionary-table-idempotent-create-crash`
  - `dictionary-drop-crash`
  - `dictionary-drop-file-op-marker-crash`
  - `dictionary-multi-drop-crash`
  - `dictionary-cross-schema-multi-drop-crash`
- Run production representative selectors:
  - `table-idempotent-ddl`
  - `ddl-broader`
  - `native-file-op-marker-drain`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- Missing-table `DROP TABLE IF EXISTS` recovers while a peer remains live and
  keeps the native file-operation marker clear.
- Existing-table `DROP TABLE IF EXISTS` recovers while a peer remains live and
  keeps the native file-operation marker set until the final peer exits.
- Missing-list `DROP TABLE IF EXISTS` recovers while a peer remains live and
  keeps the native file-operation marker clear.
- Existing-list `DROP TABLE IF EXISTS` recovers while a peer remains live and
  keeps the native file-operation marker set until the final peer exits.
- Preserved or removed table state matches `INFORMATION_SCHEMA`, direct reads,
  native file presence, ownerless/native reopen, and forced `.shm` rebuild
  expectations.
- The new recovery kind is valid in the ownerless dictionary primitive.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_primitives_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-existing-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-multi-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-multi-existing-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-table-idempotent-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-drop-file-op-marker-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-multi-drop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-cross-schema-multi-drop-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^(libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-table-idempotent-create-crash|libmylite\.ownerless-dictionary-table-idempotent-drop-crash|libmylite\.ownerless-dictionary-drop-file-op-marker-crash|libmylite\.ownerless-dictionary-implicit-drop-crash)$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test table-idempotent-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test native-file-op-marker-drain`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- This still does not prove temporary table drops or intra-MariaDB-loop crash
  points for multi-table drop lists.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
