# Ownerless Online DDL Default Lock Options

## Problem

Ownerless online DDL option coverage includes explicit no-lock and shared-lock
index variants, copy rebuilds, and instant column variants with `LOCK=NONE`.
The remaining online DDL notes still call out broader option combinations. A
small accepted MariaDB shape is explicit instant column add/drop with
`LOCK=DEFAULT`, which should still publish a dictionary generation and refresh
already-open ownerless peers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:8118-8220` handles explicit ALTER requested-lock
  clauses and treats `HA_ALTER_INPLACE_INSTANT` with `LOCK=DEFAULT` as an
  online in-place path.
- `mariadb/storage/innobase/handler/handler0alter.cc:2203-2351` documents
  `ha_innobase::check_if_supported_inplace_alter()` return classes and returns
  `HA_ALTER_INPLACE_INSTANT` for eligible no-rebuild alter shapes.
- `packages/libmylite/src/database.cc:1715-1778` wraps prepared SQL execution
  in ownerless dictionary begin/finish calls; `database.cc:7674-7712` flushes
  MariaDB/MyLite dictionary caches when a peer observes a newer stable
  generation.
- `packages/libmylite/src/ownerless_dictionary_state.cc:52-136` marks active
  DDL with an odd generation and publishes completion with the next even
  generation.

## Scope And Non-Goals

In scope:

- Extend the `online-ddl-options` selector with
  `ALTER TABLE ... ADD COLUMN ..., ALGORITHM=INSTANT, LOCK=DEFAULT`.
- Extend the same selector with
  `ALTER TABLE ... DROP COLUMN ..., ALGORITHM=INSTANT, LOCK=DEFAULT`.
- Verify an already-open peer observes the added column/default values, can
  write through it before the drop, and observes the final absence after drop.
- Keep final ownerless/native reopen checks before and after forced `.shm`
  rebuild.

Out of scope:

- Exhaust every `ALGORITHM`/`LOCK` combination MariaDB accepts.
- Partitioned, full-text, spatial, and external randomized DDL oracles.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL semantics change. The slice strengthens ownerless `ALTER TABLE`
compatibility evidence for accepted online/instant option combinations while
keeping the matrix representative rather than exhaustive.

## Database Directory And Lifecycle Impact

No directory layout changes. Native metadata stays inside the MyLite database
directory and the final state is verified through ownerless and ordinary native
reopen before and after forced `.shm` rebuild.

## Native Storage Impact

Native InnoDB executes the instant add/drop operations. MyLite must refresh peer
metadata across the ownerless dictionary generation boundary.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds focused SQL coverage and
documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `online-ddl-options` in `embedded-dev`.
- Run focused `online-ddl-options` in `ownerless-test-hooks`.
- Run adjacent `instant-column-variants` in embedded and hook builds.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- The already-open peer observes the instant `LOCK=DEFAULT` added column with
  default values for existing rows.
- The peer can update the column while it exists.
- The peer observes the column absence after the instant `LOCK=DEFAULT` drop.
- Final rows, indexes, and metadata survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- MariaDB can reject some instant option combinations for more complex table
  shapes; this slice uses a simple InnoDB table without full-text, spatial,
  partition, or indexed-virtual-column blockers.
- Broader online DDL option combinations and external randomized DDL oracles
  remain planned separately.
