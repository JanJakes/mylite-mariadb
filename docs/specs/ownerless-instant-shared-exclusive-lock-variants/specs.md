# Ownerless Instant Shared/Exclusive Lock Variants

## Problem

Ownerless instant-column coverage already proves peer refresh for
`ALGORITHM=INSTANT` ADD/DROP/reorder shapes, including `LOCK=NONE` and selected
`LOCK=DEFAULT` placements. The remaining ownerless DDL matrix still calls out
broader instant option combinations. This slice adds bounded evidence for
accepted placed stored-column instant ALTERs using `LOCK=SHARED` and
`LOCK=EXCLUSIVE` without adding another cross-process SQL selector or broad
stress run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_alter.cc:84-95` parses `LOCK=NONE`, `LOCK=SHARED`,
  `LOCK=EXCLUSIVE`, and `LOCK=DEFAULT` into explicit `Alter_info` lock states.
- `mariadb/sql/sql_alter.cc:181-216` accepts requested locks for
  `HA_ALTER_INPLACE_INSTANT` without downgrading or reporting an unsupported
  lock clause.
- `mariadb/storage/innobase/handler/handler0alter.cc:147-156` defines the
  InnoDB instant ALTER operation mask.
- `mariadb/storage/innobase/handler/handler0alter.cc:1687-1735` checks stored
  ADD/DROP/reorder eligibility in `instant_alter_column_possible()`.
- `mariadb/storage/innobase/handler/handler0alter.cc:2200-2360` classifies
  supported InnoDB instant ALTER requests through
  `ha_innobase::check_if_supported_inplace_alter()`.

## Scope And Non-Goals

In scope:

- Extend the existing `instant-column-variants` ownerless selector.
- Add one placed stored column with `ALGORITHM=INSTANT, LOCK=SHARED`.
- Add one placed stored column with `ALGORITHM=INSTANT, LOCK=EXCLUSIVE`.
- Verify already-open peer metadata refresh, default materialization, peer DML
  through the added columns, and final ownerless/native reopen before and after
  forced `.shm` rebuild.

Out of scope:

- Runtime behavior changes.
- Exhaustive instant-column type, generated-column, or partition matrices.
- SQL-level table-lock fault injection; explored SQL shapes still do not reach
  the ownerless table-wait callback.
- Broader durable DDL/file-lifecycle recovery metadata.
- External randomized MariaDB/RQG oracle execution.

## Design

The existing selector already serializes a DDL child and an already-open
ownerless peer through pipes. Reuse that selector to keep suite shape and CTest
sharding unchanged:

1. After the existing `LOCK=DEFAULT` `default_note` placement, the DDL child
   adds `shared_note INT NOT NULL DEFAULT 13 AFTER default_note` using
   `ALGORITHM=INSTANT, LOCK=SHARED`.
2. The peer verifies the new ordinal position, existing-row defaults, and DML
   through `shared_note`.
3. The child adds `exclusive_note INT NOT NULL DEFAULT 17 AFTER shared_note`
   using `ALGORITHM=INSTANT, LOCK=EXCLUSIVE`.
4. The peer verifies the new ordinal position, existing-row defaults, and DML
   through `exclusive_note`.
5. Existing rename, generated-column add/drop, ownerless/native reopen, and
   forced `.shm` rebuild checks continue over the expanded final row shape.

## Compatibility Impact

No SQL semantics or public API changes. The slice narrows the ownerless
`ALTER TABLE` partial status by adding peer-refresh evidence for accepted
instant placed stored-column ALTERs under `LOCK=SHARED` and `LOCK=EXCLUSIVE`.
It does not claim exhaustive online DDL coverage.

## Directory And Lifecycle Impact

No directory layout changes. The slice exercises native InnoDB table metadata
inside the MyLite database directory, the ownerless dictionary generation in
`concurrency/mylite-concurrency.shm`, and final recovery from durable metadata
after deleting volatile `.shm`.

## Native Storage Impact

Native InnoDB executes the instant ALTER operations. MyLite must observe the
stable ownerless dictionary generation and refresh stale table/dictionary cache
state before the already-open peer reads or writes through the new columns.

## Binary Size And Dependencies

No production binary, dependency, or license impact. The change adds only test
code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `instant-column-variants` in `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the affected embedded and hook ownerless SQL CTest shard.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the `LOCK=SHARED` and
  `LOCK=EXCLUSIVE` instant placed stored columns at the expected ordinal
  positions.
- Existing rows expose the instant default values and peer DML can update both
  columns before later DDL boundaries.
- Final row predicates include the shared/exclusive columns and pass through
  ownerless/native reopen before and after forced `.shm` rebuild.
- Docs describe this as bounded instant-lock evidence and leave broader
  randomized DDL, SQL table-wait injection, and durable file-lifecycle metadata
  as separate work.

## Risks And Follow-Up

- MariaDB may reject some instant lock combinations on more complex table
  shapes. This slice intentionally uses the same simple InnoDB table as the
  existing instant-column selector.
- Broader instant generated-column combinations, file-lifecycle recovery, and
  external randomized DDL oracles remain planned.
- A follow-up extends the same selector with virtual generated-column
  `LOCK=SHARED` add and `LOCK=EXCLUSIVE` drop coverage.
