# Ownerless Composite Direction Primary-Key DDL Crash

## Problem

Ownerless composite direction primary-key refresh coverage proves that one
process can rebuild an InnoDB clustered index from `PRIMARY(id)` to
`PRIMARY(tenant_id ASC, code DESC)` and that an already-open peer sees the new
metadata. The hook-build crash matrix already covers a plain primary-key
replacement and an idempotent primary-key no-op, but not a mixed-direction
composite clustered-key rebuild after native metadata is written and before
MyLite publishes ownerless dictionary finish.

MyLite needs a bounded crash-tail proof for this accepted primary-key variant.
If a writer dies after MariaDB completes the composite direction primary-key
rebuild but before ownerless dictionary finish, another live ownerless peer
must be able to recover the dictionary generation while retaining the native
file-operation marker until final no-live checkpoint proof, and the final
native clustered-key metadata must remain usable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` in `KEY_PART_INFO::key_part_flag` when an index column is
  declared descending.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, maps replacement primary-key metadata to
  `DICT_CLUSTERED | DICT_UNIQUE`, records per-field descending metadata, and
  treats primary-key direction changes as order-changing DDL.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes each
  key part's `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes `PRIMARY`
  metadata and key-part direction through `information_schema.statistics`.

## Scope And Non-Goals

In scope:

- Add a hook-only selector,
  `dictionary-composite-direction-primary-key-crash`, that kills a writer at
  `dictionary-before-finish` while executing
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)`.
- Verify live-peer recovery while another ownerless peer remains open.
- Verify the native file-operation marker remains retained while that peer is
  open and drains after the peer exits.
- Verify recovered `information_schema.statistics` exposes the two-part
  `PRIMARY`, with `tenant_id` ascending and `code` descending.
- Verify the old `id` column is no longer part of `PRIMARY`.
- Verify forced-index reads, duplicate composite-key rejection, writes that
  duplicate only the old `id`, and final ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Other primary-key option combinations, algorithm/lock variants, and
  alternate crash points.
- AUTO_INCREMENT primary-key replacements, which are covered by separate
  focused refresh slices.
- SQL-level table-lock wait fault injection; prior exploratory SQL shapes did
  not reach the ownerless table-wait callback.
- External randomized DDL/RQG oracle execution.

## Design

- Reuse the existing ownerless dictionary fault hook with
  `MYLITE_OWNERLESS_TEST_FAULT=dictionary-before-finish`.
- Create `app.ownerless_composite_direction_primary_key_base` with
  `PRIMARY(id)` and three rows.
- Crash the writer while it executes the composite direction primary-key
  replacement.
- Use `crash_ownerless_dictionary_writer_with_held_live_peer()` so a live peer
  proves recovery works before no-live cleanup.
- While the peer is still open, recover ownerless metadata, insert a row that
  duplicates only the old `id` but has a new composite primary-key value, and
  verify duplicate composite-key writes still fail.
- Release the peer and verify the retained native file-operation marker drains
  on the next no-live ownerless reopen.
- Reuse the existing composite direction primary-key final-state assertion for
  ownerless/native reopen before and after forced shared-memory rebuild.

## Compatibility Impact

This strengthens the partial ownerless DDL crash matrix for an accepted
clustered-index rebuild variant. SQL semantics are unchanged; ordinary
exclusive embedded behavior continues to inherit MariaDB/InnoDB behavior.

The broader ownerless compatibility claim remains partial because this is one
crash point and one primary-key option shape, not the full algorithm/lock,
concurrent-conflict, or external-oracle matrix.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The slice exercises
MariaDB/InnoDB native clustered-index metadata, MyLite's ownerless
dictionary-generation recovery path, and retained native file-operation marker
drain after the final live peer exits.

## Native Storage Impact

No native storage format changes. The test verifies recovered InnoDB metadata
and duplicate-key enforcement for the final composite direction clustered key.

## Public API Impact

No public API changes.

## Binary Size Impact

No meaningful production binary-size impact. The shared bounded primary-key
replacement classifier now accepts composite key-part lists with optional
`ASC`/`DESC`; tests and docs cover the new crash path.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused `dictionary-composite-direction-primary-key-crash` in
  `ownerless-test-hooks`.
- Run registered standalone CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-composite-direction-primary-key-crash$' --output-on-failure`.
- Run adjacent `dictionary-primary-key-crash`,
  `dictionary-primary-key-idempotent-crash`, `primary-key-ddl`,
  `descending-primary-key-ddl`, and `composite-direction-primary-key-ddl`
  selectors.
- Run the hook ownerless CTest subset, ownerless stress, `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer can recover the dead dictionary owner.
- The native file-operation marker remains set while the live peer stays open
  and drains after no-live ownerless recovery.
- Recovered ownerless and native reopen expose `PRIMARY(tenant_id ASC,
  code DESC)` and no longer expose `id` as part of `PRIMARY`.
- Duplicate composite-key inserts fail, while inserts that duplicate only the
  old `id` succeed.
- Final state survives forced `.shm` rebuild.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test mylite_embedded_ownerless_innodb_lock_hooks_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-composite-direction-primary-key-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-composite-direction-primary-key-crash$' --repeat until-fail:5 --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-crash$|^libmylite\.ownerless-dictionary-primary-key-idempotent-crash$|^libmylite\.ownerless-primitives$|^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test composite-direction-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test descending-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-tablespace-replay`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 1 --output-on-failure`
- `env LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- Broader primary-key algorithm/lock matrices, alternate crash points,
  concurrent-conflict schedules, AUTO_INCREMENT variants, and external
  randomized DDL/RQG stress remain separate work.
