# Ownerless Force-Rebuild Lock-Copy Option Order

## Problem

Ownerless force-rebuild crash recovery covers plain `ALTER TABLE ... FORCE` and
the explicit `FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE` spelling. MariaDB accepts
`LOCK=EXCLUSIVE` and `ALGORITHM=COPY` as independent `ALTER TABLE` option
items, so the reversed `FORCE, LOCK=EXCLUSIVE, ALGORITHM=COPY` spelling should
reach the same ownerless dictionary and native file-operation recovery path.

This slice adds deterministic hook-build evidence for that reversed option
order without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:8150-8165` accepts table options,
  `FORCE`, `alter_algorithm_option`, and `alter_lock_option` as ordinary
  `ALTER TABLE` list items.
- `mariadb/sql/sql_yacc.yy:8210-8232` stores requested algorithm and lock
  values through `Alter_info`.
- The existing ownerless force-rebuild crash helper already verifies the
  native file-operation marker, InnoDB table/index state, retained payloads,
  post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- `ALTER TABLE app.ownerless_force_rebuild_crash_base FORCE, LOCK=EXCLUSIVE,
  ALGORITHM=COPY`;
- live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild checks.

Out of scope:

- production ownerless code changes;
- exhaustive table-option order matrices;
- broader ALTER rebuild variants;
- external randomized DDL/RQG stress.

## Design

Add one hook-build direct selector:

```sh
mylite_ownerless_cross_process_sql_test dictionary-force-rebuild-lock-copy-crash
```

The selector reuses the existing force-rebuild crash oracle. It kills the
writer at the `dictionary-before-finish` hook after the native table-copy
rebuild has completed and verifies:

- the native file-operation checkpoint marker remains set while another
  ownerless peer is live;
- ownerless recovery sees rebuilt InnoDB table and secondary-index metadata
  with retained rows and payloads;
- after peer release, no-live recovery drains the marker;
- later ownerless writes, native reopen, and forced `.shm` rebuild preserve the
  rebuilt table.

## Compatibility Impact

No SQL grammar, public API, or storage format changes. The tests record that
the reversed explicit copy-lock option order behaves like the already-covered
`FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE` spelling for this supported rebuild
form.

## Directory And Native Storage Impact

The covered DDL is a native InnoDB table-copy rebuild. MyLite must keep the
native file-operation checkpoint marker durable while a peer is live and clear
it only after no-live checkpoint proof.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused reversed-order FORCE rebuild CTest.
- Run the adjacent FORCE and same-engine rebuild crash selector subset.
- Run the production build target for `mylite_ownerless_cross_process_sql_test`.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The reversed FORCE rebuild selector passes.
- Live-peer recovery retains the native marker.
- No-live recovery drains the native marker.
- Ownerless/native reopen and forced `.shm` rebuild preserve table/index
  metadata and rows.

## Risks And Follow-Up

- Broader ALTER rebuild variants and full randomized external-oracle stress
  remain separate completion work.
