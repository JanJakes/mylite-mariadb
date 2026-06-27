# Ownerless Descending Primary-Key DDL Crash Recovery

## Problem

Ownerless descending primary-key refresh coverage proves an already-open peer
can observe `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code DESC)`
after another ownerless process completes the native clustered-index rebuild.
The crash matrix did not separately prove that same descending replacement
primary-key shape after MariaDB/InnoDB writes the native metadata but before
MyLite publishes ownerless dictionary finish.

MyLite needs bounded crash-tail evidence for this accepted primary-key variant:
if the writer dies after the descending clustered-key rebuild, another live
ownerless peer must recover the dictionary generation, retain the native
file-operation marker until final no-live checkpoint proof, and preserve the
descending primary-key metadata and enforcement through native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` on descending key parts.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks replacement primary-key
  DDL with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, accepts replacement `DROP PRIMARY KEY` plus
  `ADD PRIMARY KEY`, and treats primary-key direction changes as clustered
  index rebuild work.
- `mariadb/storage/innobase/handler/handler0alter.cc` and
  `mariadb/storage/innobase/handler/ha_innodb.cc` carry `HA_REVERSE_SORT` into
  InnoDB descending index metadata.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes descending key
  parts through `information_schema.statistics.COLLATION = 'D'`.

## Scope And Non-Goals

In scope:

- Add a focused hook selector,
  `dictionary-descending-primary-key-crash`, that kills a writer at
  `dictionary-before-finish` while executing
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code DESC)`.
- Verify live-peer ownerless recovery while another ownerless peer remains
  open.
- Verify the native file-operation marker remains set while that peer is open
  and drains after the peer exits.
- Verify recovered `information_schema.statistics` exposes `PRIMARY(code)`
  with `NON_UNIQUE = 0` and `COLLATION = 'D'`.
- Verify the old `id` column is no longer part of `PRIMARY`.
- Verify forced-index reads, duplicate replacement-key rejection, old-key
  duplicate acceptance, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Full algorithm/lock option matrices, alternate crash points, concurrent
  duplicate-key schedules, and external randomized DDL oracles.
- Composite direction primary-key crash recovery, covered separately by
  `ownerless-composite-direction-primary-key-ddl-crash`.
- AUTO_INCREMENT descending primary-key replacement crash recovery, which has
  extra high-watermark and secondary-unique-index invariants.
- SQL-level table-lock fault injection; prior exploratory SQL shapes stopped
  before the ownerless table-wait callback.

## Design

Add a hook-only crash selector beside the existing primary-key crash cases:

1. Create `app.ownerless_descending_primary_key_base` with `PRIMARY KEY(id)`
   and seed three rows.
2. Crash a writer at `dictionary-before-finish` while it runs
   `ALTER TABLE app.ownerless_descending_primary_key_base DROP PRIMARY KEY,
   ADD PRIMARY KEY (code DESC)`.
3. Keep an ownerless peer open while a new ownerless opener recovers the
   dictionary generation.
4. Assert the native file-operation marker remains set while the live peer is
   open, and that recovered metadata exposes `PRIMARY(code DESC)`.
5. Exercise duplicate-key rejection on `code`, allow a write that duplicates
   only the old `id`, then close the recovering opener.
6. Release the peer and verify a no-live ownerless reopen drains the retained
   native file-operation marker.
7. Reuse the existing descending primary-key final-state helper for
   ownerless/native reopen before and after forced shared-memory rebuild.

The product classifier does not need another recovery kind: the bounded
primary-key replacement parser already accepts key-part lists where each part
has optional `ASC`/`DESC`.

## Compatibility Impact

This upgrades descending primary-key replacement from refresh-only evidence to
live-peer crash recovery evidence for the focused accepted SQL shape. It does
not broaden supported primary-key option matrices or claim AUTO_INCREMENT
descending crash recovery.

## Directory And Lifecycle Impact

No new MyLite files or directory layout changes. The selector exercises native
InnoDB clustered-index rebuild metadata inside the MyLite database directory
and MyLite's retained native file-operation marker lifecycle.

## Native Storage Impact

Native InnoDB storage format is unchanged. The slice verifies the native
descending clustered-index metadata remains usable after ownerless dictionary
recovery, no-live checkpoint drain, forced `.shm` rebuild, and ordinary native
reopen.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. Changes are limited to hook-only test code,
CTest registration, and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused selector:
  `mylite_ownerless_cross_process_sql_test dictionary-descending-primary-key-crash`.
- Run registered standalone CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-descending-primary-key-crash$' --output-on-failure`.
- Run adjacent hook CTests for plain/composite/idempotent primary-key crash
  coverage plus ownerless primitives and lock hooks.
- Run production selectors for `descending-primary-key-ddl`,
  `primary-key-ddl`, `composite-direction-primary-key-ddl`, and
  `primary-key-tablespace-replay`.
- Run ownerless DDL stress, the production embedded ownerless SQL subset,
  format/production-build guards, and `git diff --check`.

## Acceptance Criteria

- The focused hook selector kills the writer at `dictionary-before-finish`.
- A live peer can remain open while another ownerless opener recovers the dead
  dictionary generation.
- The native file-operation marker remains set while the live peer remains
  open and drains after no-live ownerless recovery.
- Recovered ownerless and native reopen expose `PRIMARY(code DESC)` and no
  longer expose `id` as part of `PRIMARY`.
- Duplicate replacement-key inserts fail, writes that duplicate only the old
  primary key succeed, and final state survives forced `.shm` rebuild.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-descending-primary-key-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-descending-primary-key-crash$' --repeat until-fail:5 --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-crash$|^libmylite\.ownerless-dictionary-descending-primary-key-crash$|^libmylite\.ownerless-dictionary-composite-direction-primary-key-crash$|^libmylite\.ownerless-dictionary-primary-key-idempotent-crash$|^libmylite\.ownerless-primitives$|^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test descending-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test composite-direction-primary-key-ddl`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-tablespace-replay`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 1 --output-on-failure`
- `env LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- AUTO_INCREMENT descending primary-key crash recovery remains separate because
  it combines clustered-key rebuild, secondary unique-key preservation, and
  AUTO_INCREMENT high-watermark replay.
- Broader primary-key algorithm/lock matrices, concurrent-conflict schedules,
  alternate crash points, and external MariaDB/RQG stress remain planned.
