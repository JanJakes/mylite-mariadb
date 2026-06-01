# Ownerless Instant LOCK DEFAULT Variants

## Problem

Ownerless DDL coverage already exercises instant column add/drop with
`LOCK=DEFAULT` and several instant placement, rename, and virtual-column shapes
with `LOCK=NONE`. The remaining DDL matrix still calls out broader instant
option combinations. This slice expands the existing peer-refresh test to cover
`ALGORITHM=INSTANT, LOCK=DEFAULT` on a placed stored column and an instant
column rename, then verifies ownerless and native reopen state before and after
forced `.shm` rebuild.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:10766` enters `mysql_alter_table()` for SQL
  `ALTER TABLE` execution.
- `mariadb/storage/innobase/handler/handler0alter.cc:147` defines InnoDB
  instant ALTER operations that do not call `inplace_alter_table()`.
- `mariadb/storage/innobase/handler/handler0alter.cc:493` adjusts table
  metadata for instant ADD/DROP/reorder column operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:2220` defines
  `ha_innobase::check_if_supported_inplace_alter()`, which classifies
  supported online/instant alter requests.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_instant_column_variants_refresh_peer_dictionary()` already
  coordinates a DDL process and a peer ownerless reader/writer over instant
  FIRST/AFTER placement, rename, virtual generated-column add/drop, and
  ownerless/native reopen checks.

## Scope And Non-Goals

In scope:

- Extend the existing instant-column variant test with a placed stored column
  using `ALGORITHM=INSTANT, LOCK=DEFAULT`.
- Change the existing instant rename step to use `ALGORITHM=INSTANT,
  LOCK=DEFAULT`, preserving peer-visible rename assertions.
- Verify the peer observes the new column position, default values on existing
  rows, final row values, ownerless reopen, native exclusive reopen, and forced
  `.shm` rebuild reopen.
- Update compatibility and ownerless-concurrency docs to narrow the remaining
  instant-option gap.

Out of scope:

- New runtime behavior changes.
- Full randomized online DDL oracle execution.
- Unsupported table-lock SQL mode or table-wait fault injection.
- Exhaustive instant combinations for every column type and generated-column
  placement.

## Design

The existing test already serializes a DDL child and peer ownerless connection
through pipes. The slice adds one new synchronization point after the existing
`side_value` instant placement:

- the DDL child adds `default_note INT NOT NULL DEFAULT 11 AFTER side_value`
  with `ALGORITHM=INSTANT, LOCK=DEFAULT`,
- the peer verifies `default_note` appears at ordinal position 5 and existing
  rows receive the default,
- the DDL child renames `marker` to `renamed_marker` with
  `ALGORITHM=INSTANT, LOCK=DEFAULT`,
- final assertions include `default_note` sums and per-row predicates.

This keeps the change inside the already focused ownerless instant-column
variant test and avoids duplicating setup/reopen logic.

## Compatibility Impact

No public API or runtime behavior changes. The compatibility matrix can now
claim peer-visible `LOCK=DEFAULT` coverage for instant stored-column placement
and instant column rename in addition to the previously covered instant
add/drop shapes.

## Database Directory And Lifecycle Impact

No directory layout changes. The existing test continues to verify final state
through ownerless read/write reopen, native exclusive reopen, forced `.shm`
rebuild, and native exclusive reopen after rebuild.

## Native Storage Impact

No storage format changes. The slice exercises MariaDB/InnoDB native instant
DDL metadata and default-value materialization through MyLite ownerless
dictionary refresh.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the embedded preset.
- Run focused selector
  `./build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  instant-column-variants`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused ownerless instant-column variant test passes with the new
  `LOCK=DEFAULT` placement and rename operations.
- Ownerless/native reopen checks before and after forced `.shm` rebuild still
  pass.
- Docs describe the narrowed instant-option gap without claiming exhaustive
  randomized DDL coverage.

## Risks And Follow-Up

- MariaDB may reject some instant `LOCK=DEFAULT` option combinations on future
  bases; this test intentionally stays on accepted stored-column placement and
  rename shapes for the current base.
- Broader randomized DDL oracles and additional instant type combinations
  remain separate work.
