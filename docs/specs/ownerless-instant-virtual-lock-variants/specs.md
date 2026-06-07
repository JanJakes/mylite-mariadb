# Ownerless Instant Virtual Lock Variants

## Problem

Ownerless instant-column coverage includes virtual generated-column add/drop
with `LOCK=NONE` and placed stored-column lock variants with `LOCK=DEFAULT`,
`LOCK=SHARED`, and `LOCK=EXCLUSIVE`. The remaining instant-option gap still
includes generated-column combinations under non-default lock clauses. This
slice adds focused peer-refresh evidence for an instant virtual generated
column added under `LOCK=SHARED` and dropped under `LOCK=EXCLUSIVE`, reusing the
existing fast selector instead of adding another shard case.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_alter.cc:84-95` parses the explicit ALTER lock clauses into
  `Alter_info::requested_lock`.
- `mariadb/sql/sql_alter.cc:181-216` accepts requested locks when the handler
  classifies an operation as `HA_ALTER_INPLACE_INSTANT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:147-156` includes
  `ALTER_ADD_VIRTUAL_COLUMN` and `ALTER_DROP_VIRTUAL_COLUMN` in
  `INNOBASE_ALTER_INSTANT`.
- `mariadb/storage/innobase/handler/handler0alter.cc:2200-2360` classifies
  supported InnoDB instant ALTER requests before execution.
- `mariadb/storage/innobase/handler/handler0alter.cc:5986-6025` applies native
  instant metadata changes through `innobase_instant_try()`.

## Scope And Non-Goals

In scope:

- Extend `instant-column-variants` with a virtual generated column added using
  `ALGORITHM=INSTANT, LOCK=SHARED`.
- Drop that virtual generated column using `ALGORITHM=INSTANT, LOCK=EXCLUSIVE`.
- Verify already-open peer metadata refresh, generated expression values while
  the column exists, absence and stale-name rejection after drop, and final
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Runtime changes.
- Exhaustive generated-column expression or indexed virtual-column matrices.
- SQL-level table-lock fault injection.
- Broader durable DDL/file-lifecycle recovery metadata.
- External randomized MariaDB/RQG DDL oracles.

## Design

The existing `instant-column-variants` selector already coordinates a DDL child
and an already-open ownerless peer. Add two synchronization points after the
stored-column rename step:

1. The DDL child adds `shared_value_total` as
   `base_value + shared_note` with `ALGORITHM=INSTANT, LOCK=SHARED`.
2. The peer verifies `INFORMATION_SCHEMA.COLUMNS` shows the virtual column and
   reads the generated aggregate.
3. The DDL child drops `shared_value_total` with
   `ALGORITHM=INSTANT, LOCK=EXCLUSIVE`.
4. The peer verifies the column is absent and stale reads fail.
5. Existing virtual `value_double` `LOCK=NONE` add/drop and final reopen checks
   continue unchanged, with final absence checks covering both virtual columns.

## Compatibility Impact

No public API or runtime behavior changes. The slice narrows ownerless
`ALTER TABLE` partial status for accepted instant virtual generated-column
lock variants while keeping broader online DDL and randomized oracle coverage
partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector still verifies durable native table
metadata through ownerless read/write reopen, ordinary native exclusive reopen,
forced `.shm` rebuild, and native reopen after rebuild.

## Native Storage Impact

Native InnoDB executes the instant virtual-column metadata updates. MyLite must
refresh stale ownerless peer dictionary state across the published dictionary
generation before the peer reads the generated column or observes its absence.

## Binary Size And Dependencies

No production binary, dependency, or license impact. The change adds only test
code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `instant-column-variants` in `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The already-open peer observes `shared_value_total` after the
  `LOCK=SHARED` instant add and reads the expected generated aggregate.
- The peer observes the column absent after the `LOCK=EXCLUSIVE` instant drop
  and stale column reads fail.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild
  still pass.
- Docs describe the added evidence without claiming exhaustive instant DDL,
  table-lock fault-injection, or broader file-lifecycle recovery coverage.

## Risks And Follow-Up

- More complex virtual generated-column shapes, indexed virtual columns, and
  expressions using rejected functions remain separate coverage.
- The broad online DDL option matrix and randomized external oracles remain
  planned.
