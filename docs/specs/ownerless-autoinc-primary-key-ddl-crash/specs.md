# Ownerless AUTO_INCREMENT Primary-Key DDL Crash Recovery

## Problem

Ownerless AUTO_INCREMENT primary-key refresh coverage proves that an
already-open peer can observe a table whose primary key moved away from an
existing AUTO_INCREMENT column while a unique secondary index keeps that column
valid for allocation. The crash matrix did not prove the same clustered-key
rebuild after MariaDB/InnoDB writes native metadata but before MyLite publishes
ownerless dictionary finish.

This shape is riskier than ordinary primary-key replacement: recovery must
preserve replacement primary-key metadata, the retained unique secondary index
on the AUTO_INCREMENT column, the native file-operation checkpoint marker, and
the high-watermark gap left by a duplicate-key failure.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` requires an
  AUTO_INCREMENT column to participate in a key.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks replacement primary-key
  DDL with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, accepts replacement `DROP PRIMARY KEY` plus
  `ADD PRIMARY KEY`, and handles added unique indexes in the same ALTER.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` reserves an
  AUTO_INCREMENT value before row insertion, so a later duplicate-key failure
  can leave a gap that must not be reused.
- `mariadb/storage/innobase/handler/ha_innodb.cc:get_auto_increment()` reads
  native InnoDB AUTO_INCREMENT state; MyLite's ownerless AUTO_INCREMENT
  registry must seed/publish a monotonic high watermark across processes.
- `mariadb/storage/innobase/row/row0ins.cc` persists
  `PAGE_ROOT_AUTO_INC` during clustered insert work.

## Scope And Non-Goals

In scope:

- Broaden the bounded primary-key replacement recovery classifier to accept
  the exact AUTO_INCREMENT-preserving ALTER shape used by the refresh selector:
  `DROP PRIMARY KEY`, `DROP INDEX <old-code-key>`,
  `ADD UNIQUE KEY <id-key> (id)`, `ADD PRIMARY KEY (code)`.
- Add a focused hook selector,
  `dictionary-primary-key-autoinc-crash`, that kills the writer at
  `dictionary-before-finish`.
- Verify live-peer recovery while another ownerless peer remains open and
  native file-operation marker retention until final no-live drain.
- Verify recovered metadata exposes `PRIMARY(code)` and the retained unique
  secondary key on `id`.
- Verify implicit insert allocation receives `id = 3`, duplicate `code`
  rejection consumes but does not reuse `id = 4`, and a post-rebuild insert
  after forced `.shm` rebuild receives `id = 5`.

Out of scope:

- AUTO_INCREMENT descending primary-key crash recovery, composite
  AUTO_INCREMENT primary-key replacements, algorithm/lock options, ignored or
  invisible indexes, alternate crash points, concurrent conflict schedules, and
  external randomized DDL/RQG oracles.
- SQL-level table-lock fault injection; prior exploratory SQL shapes stopped
  before the ownerless table-wait callback.

## Design

The classifier remains intentionally bounded:

1. It first recognizes the existing primary-key replacement prefix,
   `ALTER TABLE <table> DROP PRIMARY KEY,`.
2. It continues to accept the existing direct `ADD PRIMARY KEY (<parts>)`
   grammar for ordinary, descending, and composite primary-key replacements.
3. It additionally accepts one intermediate action group:
   `DROP INDEX|KEY <existing-index>, ADD UNIQUE KEY|INDEX <new-index> (<parts>),`
   before the final `ADD PRIMARY KEY (<parts>)`.
4. Pre-execution metadata must prove the table has `PRIMARY`, the dropped
   index exists, the added unique-index name is absent, and every referenced
   key-part column exists.
5. Prefix lengths, unnamed added unique keys, generated option tails, trailing
   ALTER options, and alternate action ordering remain unclassified.

The hook test creates `app.ownerless_pk_autoinc` with
`id INT AUTO_INCREMENT PRIMARY KEY`, a unique `code` key, and two rows. It
crashes the writer during the primary-key replacement, recovers while a peer
remains open, inserts one implicit row, verifies a duplicate `code` write fails,
then releases the peer and proves no-live recovery drains the native marker.
Final ownerless/native reopen and forced `.shm` rebuild checks reuse the
existing AUTO_INCREMENT primary-key state helper before a final insert proves
the duplicate-failure gap remains durable.

## Compatibility Impact

This upgrades the focused AUTO_INCREMENT primary-key replacement from
refresh-only evidence to live-peer crash recovery evidence. It does not broaden
the supported SQL option matrix beyond the documented deterministic ALTER
shape.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The selector exercises native
InnoDB clustered-index rebuild metadata, MyLite ownerless dictionary recovery,
the native file-operation checkpoint marker, and the existing shared
AUTO_INCREMENT registry lifecycle inside the MyLite database directory.

## Native Storage Impact

Native InnoDB storage format is unchanged. The slice verifies recovered native
metadata, native AUTO_INCREMENT high-watermark persistence across no-live
checkpoint drain, forced `.shm` rebuild, and ordinary native reopen.

## Public API Impact

No public API changes.

## Binary Size Impact

No meaningful production binary-size impact. The product classifier accepts one
additional bounded ALTER shape; tests and docs carry the main diff.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused selector:
  `mylite_ownerless_cross_process_sql_test dictionary-primary-key-autoinc-crash`.
- Run registered standalone CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-autoinc-crash$' --output-on-failure`.
- Run adjacent hook CTests for primary-key crash variants, ownerless
  primitives, and lock hooks.
- Run production selectors for `primary-key-autoinc-ddl`,
  `primary-key-ddl`, `descending-primary-key-ddl`,
  `composite-direction-primary-key-ddl`, `auto-inc`, `auto-inc-ddl`,
  `auto-inc-column-ddl`, and `primary-key-tablespace-replay`.
- Run ownerless DDL stress, the production embedded ownerless SQL subset,
  format/production-build guards, and `git diff --check`.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer can remain open while another ownerless opener recovers the dead
  dictionary generation.
- The native file-operation marker remains set while the live peer remains
  open and drains after no-live ownerless recovery.
- Recovered metadata exposes `PRIMARY(code)` and
  `ownerless_pk_autoinc_id_key(id)`.
- The first post-recovery implicit insert receives `id = 3`.
- A duplicate replacement-key insert fails and the later post-`.shm` rebuild
  insert receives `id = 5`, proving `id = 4` was not reused.
- Ownerless and native exclusive reopen preserve row counts, sums, max ID, and
  key metadata.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-primary-key-autoinc-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-autoinc-crash$' --repeat until-fail:5 --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-crash$|^libmylite\.ownerless-dictionary-descending-primary-key-crash$|^libmylite\.ownerless-dictionary-composite-direction-primary-key-crash$|^libmylite\.ownerless-dictionary-primary-key-autoinc-crash$|^libmylite\.ownerless-dictionary-primary-key-idempotent-crash$|^libmylite\.ownerless-primitives$|^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-autoinc-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test descending-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test composite-direction-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test auto-inc-column-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-tablespace-replay`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 1 --output-on-failure`
- `env LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- AUTO_INCREMENT descending primary-key crash recovery remains separate.
- Broader primary-key option matrices, alternate action ordering, alternate
  crash points, concurrent-conflict schedules, and external MariaDB/RQG stress
  remain planned.
