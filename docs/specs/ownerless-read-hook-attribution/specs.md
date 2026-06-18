# Ownerless Read Hook Attribution

## Problem

The ownerless point-select probe showed that real InnoDB indexed reads are much
slower than ordinary reads, while page-version read hooks and WAL scans remain
zero in the reduced sample. The remaining time is inside native
`mysql_query()`/`mysql_stmt_execute()` execution, so the production probe needs
to split ownerless MDL, transaction, and read-view callback cost before changing
read refresh or hook policy.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/mdl.cc` calls `mylite_ownerless_mdl_acquire()` and
  `mylite_ownerless_mdl_release()` when ownerless MDL hooks are enabled.
- `mariadb/storage/innobase/include/trx0sys.h` calls ownerless transaction
  hooks from transaction ID allocation, read-view snapshot, transaction number
  assignment, read/write registration, and deregistration paths.
- `mariadb/storage/innobase/read/read0read.cc` publishes ownerless read views
  from `ReadView::publish_ownerless()`, closes them when the view ends, and
  calls the ownerless read-view snapshot hook from
  `trx_sys_t::clone_oldest_view()`.
- `packages/libmylite/src/database.cc` owns the hook callback bodies used by
  the embedded runtime. Direct SQL reaches those hooks inside `mysql_query()`
  from `exec_result_impl()`, and prepared SQL reaches them inside
  `mysql_stmt_execute()` from `mylite_step()`.

## Design

Extend the existing `mylite_ownerless_database_*` performance counter block with
count and elapsed-time pairs for ownerless native read callbacks:

- MDL acquire and release;
- transaction allocate, register, assign-number, deregister, and snapshot;
- read-view register, deregister, and snapshot.

Use a counted scope that checks the ownerless database perf flag once per
callback. When stats are disabled, the callback does not take timestamps or
update atomics. When `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, the
embedded production probe already resets/enables the database perf counters
around tableless and point-select read sections, so the new counters can be
reported from the same probe windows.

The probe mirror enum must stay in the exact same order as the C++ counter enum.
The stats-enabled probe should emit both raw counters and compact per-select
summary keys for ownerless direct/prepared tableless reads and ownerless
direct/prepared InnoDB point selects.

## Scope And Non-Goals

In scope:

- diagnostic counters in the existing first-party ownerless database perf
  family;
- production embedded performance probe output under the existing attribution
  flag;
- documentation recording that the slice identifies where read overhead sits.

Out of scope:

- skipping MDL, transaction, or read-view publication;
- changing ownerless snapshot, dictionary, page-version, or active-reader
  policy;
- changing SQL behavior, public C API behavior, or durable directory layout;
- adding pass/fail timing thresholds for the new counters.

## Compatibility And Storage Impact

No MySQL/MariaDB SQL behavior, public MyLite API behavior, native storage
format, or ownerless coordination semantics change. The counters are diagnostic
only. Ordinary stats-off execution keeps using the same MariaDB hook paths and
the same MyLite database directory layout.

## Test Plan

- Build `mylite_embedded_performance_probe` in `php-embedded-prod`.
- Run a reduced production probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify tableless and
  point-select hook summary keys are present.
- Run focused ownerless selectors for the tableless read fast path and
  visible-fast insert safety.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Ownerless database perf stats expose count and elapsed-time counters for MDL,
  transaction, and read-view callbacks.
- Stats-enabled probe output includes per-select native hook summaries for
  ownerless direct/prepared tableless and InnoDB point-select reads.
- The slice remains diagnostic-only and does not alter ownerless correctness
  policy.

## Implementation Evidence

A reduced local `php-embedded-prod` attribution run on 2026-06-18 used:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=100 \
MYLITE_PERF_INSERT_ITERATIONS=8 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The run emitted the new tableless and point-select MDL, transaction, and
read-view hook summary keys. Tableless direct/prepared `SELECT 1` had zero
ownerless MDL, transaction, and read-view hooks. Ownerless direct point selects
reported `1.000` MDL acquire/release calls per select, `1.020` transaction
snapshot calls per select, `1.000` read-view register/deregister calls per
select, zero page-version reads, and `0.312 ms/select` direct `mysql_query()`
time. Ownerless prepared point selects reported `1.010` MDL acquire/release
calls per select, `1.000` transaction snapshot calls per select, `1.000`
read-view register/deregister calls per select, zero page-version reads,
`0.035 ms/select` ownerless refresh time, and `0.281 ms/select` native
`mysql_stmt_execute()` time. The sampled hook elapsed time was about
`0.008-0.011 ms/select`, so this slice points the next read optimization away
from registry callback body cost and toward native execute/read-view creation
or statement-boundary refresh policy.

## Risks

The new counters identify callback cost, but they do not by themselves prove a
safe optimization. Any later fast path that skips or batches MDL, transaction,
or read-view publication must separately prove peer join/leave behavior,
snapshot visibility, DDL blocking, and active-reader reclamation safety.
