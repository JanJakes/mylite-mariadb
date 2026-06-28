# Ownerless AUTO_INCREMENT Descending Primary-Key DDL Crash Recovery

## Problem

Ownerless AUTO_INCREMENT descending primary-key refresh coverage proves an
already-open peer can observe a table whose primary key moved from
`id AUTO_INCREMENT` to `code DESC`, while a unique secondary index keeps the
AUTO_INCREMENT column valid for allocation. The previous crash slice covered
the non-descending AUTO_INCREMENT replacement; the descending key-part metadata
still needs the same live-peer crash-tail proof.

MyLite needs bounded evidence that a writer killed after native clustered-key
rebuild but before ownerless dictionary finish can be recovered while another
ownerless peer remains live, with descending `PRIMARY` metadata, retained
AUTO_INCREMENT-column unique metadata, native marker retention/drain, and
non-reused AUTO_INCREMENT gaps all preserved.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` requires an
  AUTO_INCREMENT column to participate in a key and stores `HA_REVERSE_SORT`
  for descending key parts.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks replacement primary-key
  DDL with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` accepts replacement
  `DROP PRIMARY KEY` plus `ADD PRIMARY KEY`, handles added unique indexes in
  the same ALTER, and records descending metadata for rebuilt indexes.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` carries
  `HA_REVERSE_SORT` into InnoDB index fields.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes descending key
  parts through `information_schema.statistics.COLLATION = 'D'`.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` reserves an
  AUTO_INCREMENT value before row insertion, so a later duplicate-key failure
  can leave a gap that must not be reused.

## Scope And Non-Goals

In scope:

- Add a focused hook selector,
  `dictionary-primary-key-autoinc-descending-crash`, that kills a writer at
  `dictionary-before-finish` while executing the existing
  `primary-key-autoinc-descending-ddl` ALTER shape.
- Verify live-peer recovery while another ownerless peer remains open.
- Verify the native file-operation marker remains set while the peer is open
  and drains after final no-live recovery.
- Verify recovered metadata exposes `PRIMARY(code DESC)` and retained unique
  secondary key metadata on `id`.
- Verify implicit insert allocation receives `id = 3`, duplicate `code`
  rejection consumes but does not reuse `id = 4`, and a post-rebuild insert
  after forced `.shm` rebuild receives `id = 5`.

Out of scope:

- Composite AUTO_INCREMENT primary-key crash recovery is covered by a separate
  focused slice; broader algorithm/lock options, ignored or invisible indexes,
  alternate crash points, concurrent conflict schedules, and external
  randomized DDL/RQG oracles remain out of scope here.
- SQL-level table-lock fault injection; prior exploratory SQL shapes stopped
  before the ownerless table-wait callback.

## Design

Reuse the bounded primary-key replacement parser added for the non-descending
AUTO_INCREMENT crash slice. The parser already accepts an optional
`DROP INDEX|KEY <existing-index>, ADD UNIQUE KEY|INDEX <new-index> (<parts>),`
group before the final `ADD PRIMARY KEY (<parts>)`, and the key-part parser
already accepts optional `ASC`/`DESC`.

The hook test creates `app.ownerless_pk_autoinc_desc` with
`id INT AUTO_INCREMENT PRIMARY KEY`, a unique `code` key, and two rows. It
crashes a writer running:

```sql
ALTER TABLE app.ownerless_pk_autoinc_desc
  DROP PRIMARY KEY,
  DROP INDEX ownerless_pk_autoinc_desc_code_key,
  ADD UNIQUE KEY ownerless_pk_autoinc_desc_id_key (id),
  ADD PRIMARY KEY (code DESC)
```

Recovery then proves live-peer dictionary cleanup, native marker retention and
drain, `PRIMARY(code)` with `COLLATION = 'D'`, retained unique `id` key
metadata, implicit allocation, duplicate-key gap preservation, ownerless/native
reopen, and forced `.shm` rebuild.

## Compatibility Impact

This upgrades the focused AUTO_INCREMENT descending primary-key replacement
from refresh-only evidence to live-peer crash recovery evidence. It does not
broaden the supported SQL option matrix beyond the documented deterministic
ALTER shape.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The selector exercises native
InnoDB clustered-index rebuild metadata, MyLite ownerless dictionary recovery,
the native file-operation checkpoint marker, and the existing shared
AUTO_INCREMENT registry lifecycle inside the MyLite database directory.

## Native Storage Impact

Native InnoDB storage format is unchanged. The slice verifies recovered native
descending clustered-key metadata and native AUTO_INCREMENT high-watermark
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
  `mylite_ownerless_cross_process_sql_test dictionary-primary-key-autoinc-descending-crash`.
- Run registered standalone CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-autoinc-descending-crash$' --output-on-failure`.
- Run adjacent hook CTests for primary-key crash variants, ownerless
  primitives, and lock hooks.
- Run production selectors for `primary-key-autoinc-descending-ddl`,
  `primary-key-autoinc-ddl`, `descending-primary-key-ddl`,
  `primary-key-ddl`, `auto-inc`, `auto-inc-ddl`,
  `auto-inc-column-ddl`, and `primary-key-tablespace-replay`.
- Run ownerless DDL stress, the production embedded ownerless SQL subset,
  format/production-build guards, and `git diff --check`.

## Verification Results

Accepted local verification for this slice:

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  dictionary-primary-key-autoinc-descending-crash`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-dictionary-primary-key-autoinc-descending-crash$'
  --repeat until-fail:5 --output-on-failure`
- Adjacent hook CTest group covering primary-key crash variants, ownerless
  primitives, and embedded ownerless InnoDB lock hooks.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2`
- Production direct selectors, run serially:
  `primary-key-autoinc-descending-ddl`, `primary-key-autoinc-ddl`,
  `primary-key-ddl`, `descending-primary-key-ddl`,
  `composite-direction-primary-key-ddl`, `auto-inc`, `auto-inc-ddl`,
  `auto-inc-column-ddl`, and `primary-key-tablespace-replay`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed on rerun in `68.38` seconds after an earlier non-reproduced
  `900`-second timeout. A one-round direct stress check also passed, and the
  exact eight-round direct stress shape exited successfully in `73` seconds.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 1
  --output-on-failure` passed all 16 tests in `334.97` seconds.
- `env LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `git diff --check`

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer can remain open while another ownerless opener recovers the dead
  dictionary generation.
- The native file-operation marker remains set while the live peer remains
  open and drains after no-live ownerless recovery.
- Recovered metadata exposes `PRIMARY(code DESC)` and
  `ownerless_pk_autoinc_desc_id_key(id)`.
- The first post-recovery implicit insert receives `id = 3`.
- A duplicate replacement-key insert fails and the later post-`.shm` rebuild
  insert receives `id = 5`, proving `id = 4` was not reused.
- Ownerless and native exclusive reopen preserve row counts, sums, max ID, key
  metadata, and descending primary-key direction metadata.

## Risks And Follow-Up

- Broader primary-key option matrices, alternate action ordering, alternate
  crash points, concurrent-conflict schedules, and external MariaDB/RQG stress
  remain planned.
