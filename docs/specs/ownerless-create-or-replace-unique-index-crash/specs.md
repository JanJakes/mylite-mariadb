# Ownerless CREATE OR REPLACE UNIQUE INDEX Crash

## Problem

Ownerless `CREATE OR REPLACE UNIQUE INDEX` live-peer coverage proves metadata
and duplicate-key enforcement move from the original key definition to the
replacement key definition. The crash-sensitive gap is a killed writer after
MariaDB/InnoDB has completed the native replacement but before MyLite publishes
the ownerless dictionary generation as stable.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `create_or_replace UNIQUE INDEX` and calls
  `Lex->add_create_index(Key::UNIQUE, ...)`.
- `mariadb/sql/sql_lex.h:add_create_index()` carries the `OR REPLACE` DDL
  option on the new `Key`.
- `mariadb/sql/sql_table.cc` handles `key->or_replace()` by adding the
  existing key to the alter drop list before adding the replacement key.
- MyLite `packages/libmylite/src/database.cc` begins ownerless dictionary DDL
  state before executing MariaDB DDL and the unsafe `dictionary-before-finish`
  hook pauses after successful native DDL execution but before publishing the
  stable ownerless dictionary generation.

## Design

Add a hook-only selector,
`dictionary-unique-index-replace-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `app.ownerless_unique_index_replace_crash_base`,
- creates `ownerless_unique_replace_crash_idx` over `(tenant_id, slug)`,
- verifies duplicate `(tenant_id, slug)` writes fail before replacement,
- holds another ownerless peer open,
- executes
  `CREATE OR REPLACE UNIQUE INDEX ownerless_unique_replace_crash_idx ON ...
  (tenant_id, weight)` under the `dictionary-before-finish` fault,
- kills the writer at the hook,
- verifies a new ownerless opener recovers while the live peer remains open,
- verifies the native file-operation marker remains set while the peer is live
  and drains after the peer exits,
- verifies the recovered index no longer includes `slug`, includes `weight`,
  permits the formerly duplicate old-key shape, and rejects duplicate
  replacement-key writes,
- verifies final state through ownerless reopen, ordinary native reopen,
  forced `.shm` rebuild, and native reopen after rebuild.

## Compatibility Impact

No new SQL surface is enabled. This adds crash-recovery evidence for a
supported MariaDB standalone unique-index replacement spelling in ownerless
read/write mode.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB index metadata
inside the MyLite database directory plus MyLite ownerless process-slot cleanup,
live-peer dictionary-generation recovery, final no-live marker drain, and
volatile shared-memory rebuild.

## Native Storage Impact

No storage format changes. MyLite relies on MariaDB/InnoDB native unique
secondary-index replacement and verifies recovered metadata/enforcement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run the focused `dictionary-unique-index-replace-crash` selector.
- Run the hook CTest shard containing the new unsafe selector.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The selector reaches the `dictionary-before-finish` hook and kills the writer.
- Recovery succeeds while another ownerless peer remains live.
- The native file-operation marker stays set while that peer is live and drains
  after final no-live recovery.
- Recovered metadata has the replacement unique index over `(tenant_id, weight)`
  and no `slug` key part.
- Recovered enforcement permits duplicate `(tenant_id, slug)` rows and rejects
  duplicate `(tenant_id, weight)` rows.
- Ownerless and ordinary native reopen preserve the recovered state before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- This deterministic hook selector does not replace randomized external DDL
  crash exploration.
- Broader algorithm/lock-option replacement variants remain planned.
