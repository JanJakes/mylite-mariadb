# Ownerless AUTO_INCREMENT Composite Primary-Key DDL Crash Recovery

## Problem

Ownerless AUTO_INCREMENT primary-key crash coverage proves a table can recover
after moving `PRIMARY` away from `id AUTO_INCREMENT` while retaining a unique
secondary key on `id`. Descending-key crash coverage proves the same path keeps
descending key-part metadata and AUTO_INCREMENT allocation gaps. The remaining
bounded gap is the combined shape: a clustered-key rebuild that keeps
AUTO_INCREMENT valid through a retained unique key while replacing the primary
key with a composite mixed-direction key.

MyLite needs evidence that a writer killed after native InnoDB clustered-key
rebuild but before ownerless dictionary finish can be recovered while another
ownerless peer remains live, with composite key order, key-part direction,
retained AUTO_INCREMENT unique metadata, native marker retention/drain, and
non-reused AUTO_INCREMENT gaps preserved.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` requires an
  AUTO_INCREMENT column to participate in a key and stores key-part direction
  metadata.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks replacement primary-key
  DDL with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` accepts replacement
  `DROP PRIMARY KEY` plus `ADD PRIMARY KEY`, handles added unique indexes in
  the same ALTER, and records rebuilt clustered-index metadata.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` carries
  descending key-part metadata into InnoDB index fields.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes key-part order
  and direction through `information_schema.statistics.SEQ_IN_INDEX` and
  `COLLATION`.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` reserves an
  AUTO_INCREMENT value before row insertion, so a later duplicate-key failure
  can leave a gap that must not be reused.

## Scope And Non-Goals

In scope:

- Add a focused hook selector,
  `dictionary-primary-key-autoinc-composite-crash`, that kills a writer at
  `dictionary-before-finish`.
- Use the deterministic ALTER shape:
  `DROP PRIMARY KEY`, `DROP INDEX <old-composite-key>`,
  `ADD UNIQUE KEY <id-key> (id)`,
  `ADD PRIMARY KEY (tenant_id ASC, code DESC)`.
- Verify live-peer recovery while another ownerless peer remains open.
- Verify the native file-operation marker remains set while the peer is open
  and drains after final no-live recovery.
- Verify recovered metadata exposes `PRIMARY(tenant_id ASC, code DESC)` and a
  retained unique secondary key on `id`.
- Verify implicit insert allocation receives `id = 3`, duplicate composite-key
  rejection consumes but does not reuse `id = 4`, and a post-rebuild insert
  after forced `.shm` rebuild receives `id = 5`.

Out of scope:

- Additional ALTER action orderings, algorithm/lock options, ignored or
  invisible indexes, nullable key variants, alternate crash points, concurrent
  conflict schedules, and external randomized DDL/RQG oracles.
- SQL-level table-lock fault injection; prior exploratory SQL shapes stopped
  before the ownerless table-wait callback.

## Design

Reuse the bounded primary-key replacement parser from the AUTO_INCREMENT and
descending AUTO_INCREMENT crash slices. The parser already accepts an optional
`DROP INDEX|KEY <existing-index>, ADD UNIQUE KEY|INDEX <new-index> (<parts>),`
group before the final `ADD PRIMARY KEY (<parts>)`, and the key-part parser
already accepts comma-separated key parts with optional `ASC`/`DESC`.

The hook test creates `app.ownerless_pk_autoinc_composite` with
`id INT AUTO_INCREMENT PRIMARY KEY`, a unique `(tenant_id, code)` key, and two
rows. It crashes a writer running:

```sql
ALTER TABLE app.ownerless_pk_autoinc_composite
  DROP PRIMARY KEY,
  DROP INDEX ownerless_pk_autoinc_composite_code_key,
  ADD UNIQUE KEY ownerless_pk_autoinc_composite_id_key (id),
  ADD PRIMARY KEY (tenant_id ASC, code DESC)
```

Recovery then proves live-peer dictionary cleanup, native marker retention and
drain, `PRIMARY(tenant_id ASC, code DESC)`, retained unique `id` key metadata,
implicit allocation, duplicate-key gap preservation, ownerless/native reopen,
and forced `.shm` rebuild.

## Compatibility Impact

This upgrades the focused composite AUTO_INCREMENT primary-key replacement
from planned coverage to live-peer crash recovery evidence for one documented
deterministic ALTER shape. It does not broaden the supported SQL option matrix
beyond that shape.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The selector exercises native
InnoDB clustered-index rebuild metadata, MyLite ownerless dictionary recovery,
the native file-operation checkpoint marker, and the existing shared
AUTO_INCREMENT registry lifecycle inside the MyLite database directory.

## Native Storage Impact

Native InnoDB storage format is unchanged. The slice verifies recovered native
composite clustered-key metadata and native AUTO_INCREMENT high-watermark
persistence across no-live checkpoint drain, forced `.shm` rebuild, and
ordinary native reopen.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond hook-only test code, CTest
registration, and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused selector:
  `mylite_ownerless_cross_process_sql_test dictionary-primary-key-autoinc-composite-crash`.
- Run registered standalone CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-autoinc-composite-crash$' --output-on-failure`.
- Run adjacent hook CTests for primary-key crash variants, ownerless
  primitives, and lock hooks.
- Run production selectors for `primary-key-autoinc-ddl`,
  `primary-key-autoinc-descending-ddl`, `composite-direction-primary-key-ddl`,
  `primary-key-ddl`, `auto-inc`, `auto-inc-ddl`,
  `auto-inc-column-ddl`, and `primary-key-tablespace-replay`.
- Run ownerless DDL stress, production build guards, format checks, and
  `git diff --check`.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-primary-key-autoinc-composite-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-autoinc-composite-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(primary-key|descending-primary-key|composite-direction-primary-key|primary-key-autoinc|primary-key-autoinc-descending|primary-key-autoinc-composite|primary-key-idempotent)-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks)$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-autoinc-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-autoinc-descending-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test composite-direction-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc-column-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-tablespace-replay`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset format-check`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

Rerun notes:

- The adjacent primary-key hook CTest group first hit a non-reproduced
  pre-existing `ownerless-dictionary-primary-key-autoinc-crash` marker
  retention assertion. The isolated CTest passed, and the full adjacent group
  rerun passed.
- The focused ownerless DDL stress CTest first hit the known intermittent
  InnoDB open failure with `Data structure corruption`/`Read only transaction`.
  The isolated rerun passed after removing the failed temp root.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer can remain open while another ownerless opener recovers the dead
  dictionary generation.
- The native file-operation marker remains set while the live peer remains
  open and drains after no-live ownerless recovery.
- Recovered metadata exposes `PRIMARY(tenant_id ASC, code DESC)` and
  `ownerless_pk_autoinc_composite_id_key(id)`.
- The first post-recovery implicit insert receives `id = 3`.
- A duplicate replacement-key insert fails and the later post-`.shm` rebuild
  insert receives `id = 5`, proving `id = 4` was not reused.
- Ownerless and native exclusive reopen preserve row counts, sums, max ID, key
  metadata, and composite primary-key direction metadata.

## Risks And Follow-Up

- Broader primary-key option matrices, alternate action ordering, alternate
  crash points, concurrent-conflict schedules, and external MariaDB/RQG stress
  remain planned.
