# Ownerless Record-Wait Attribution

## Problem

The ownerless bulk-insert performance profile had a blind spot in the InnoDB
insert record-lock check path. Existing database perf counters timed explicit
ownerless record lock acquire/release hooks, but the hot multi-row insert path
reported `record_lock_acquire_calls=0` while MariaDB deep counters showed time
under `row_ins_btr_lock_undo_rec_lock`.

Before changing lock semantics or adding a single-owner bypass, MyLite needs
production-build attribution for the record availability probe that InnoDB runs
before granting insert-intention record locks.

## Source Findings

- MariaDB base: 11.8 LTS import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/lock/lock0lock.cc`:
  `lock_rec_insert_check_and_lock()` computes the insert-intention
  `LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION` mode, checks local InnoDB
  conflicts, then calls
  `mylite_ownerless_innodb_lock_wait_until_record_available_for_grant()` when
  ownerless lock hooks are enabled. A timeout from that availability probe
  enqueues the external ownerless record wait.
- `packages/libmylite/src/database.cc`:
  `ownerless_innodb_lock_wait_until_record_hook()` delegates to
  `mylite_ownerless_innodb_lock_registry_wait_until_record_available()` or the
  cycle-registry variant when page-write locks are also mapped. This is a
  different path from `ownerless_innodb_lock_acquire_record_hook()`, so the
  existing explicit acquire counters do not describe insert-intention
  availability probes.

## Design

- Add first-party database perf counters for
  `record_lock_wait_until_calls`, elapsed nanoseconds, OK results, timeouts,
  unavailable results, and errors.
- Keep stats-off production overhead bounded: the hook loads the enabled flag
  once, performs no counter atomics while disabled, and records result/elapsed
  counters only when database perf stats are enabled.
- Emit raw probe keys from `emit_database_perf_stats()`.
- Emit compact per-insert, per-row, per-statement, and bulk first/remaining
  summary keys so CI timing artifacts can distinguish:
  - explicit record acquire/release,
  - record availability probes,
  - InnoDB deep `row_ins_btr_lock_undo_rec_lock`, and
  - larger row/undo/page-write phases.
- Update the fixed-index database perf mirror in
  `ownerless_cross_process_sql_test.c` because the new counters are inserted
  before later fixed anchors.

## Compatibility And Storage Impact

This is diagnostic-only. It does not change SQL behavior, lock compatibility,
wait timeout mapping, wait-cycle detection, record-lock publication, recovery,
native InnoDB storage layout, database-directory layout, public C API, or
wire-protocol behavior.

## Verification Plan

- Build production embedded performance and ownerless SQL targets.
- Run focused ownerless lock hook and live SQL selectors.
- Run a stats-enabled production bulk probe with the same 16K-row statement
  shape as the current performance investigation.
- Run format and diff checks.

## Evidence

Final local production attribution command:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=20 \
MYLITE_PERF_INSERT_ITERATIONS=32768 \
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe \
  > /tmp/mylite-perf-record-wait-attribution-final.log
```

Key final metrics from that run:

- Ownerless explicit transaction insert: `32768` record wait-until calls,
  `79.570 ms` total, all OK.
- Ownerless autocommit insert: `32768` calls, `60.354 ms` total, all OK.
- Ownerless bulk autocommit: `16384` calls, `24.478 ms` total, all OK.
- Bulk remaining statement: `16384.000` record wait-until calls per statement,
  `24.478 ms` per statement, zero timeouts, unavailable results, or errors.
- In the same remaining bulk statement, ownerless
  `row_ins_btr_lock_undo_rec_lock` was `29.046 ms` per statement, ordinary was
  `2.399 ms`, and ownerless-minus-ordinary was `26.646 ms`.
- Larger remaining gaps were still outside the record-wait hook:
  ownerless-minus-ordinary `row_insert` was `190.280 ms` per statement,
  `trx_undo_report` was `76.220 ms`, `trx_undo_report_mtr_commit` was
  `70.608 ms`, and ownerless page-write commit-log work was `58.450 ms`.
- Throughput in this stats-enabled sample was ownerless bulk rows
  `77093.10 ops/s` versus ordinary `106934.31 ops/s`, ratio `0.7209`; the
  remaining-statement ratio was `0.4199`.

## Acceptance Criteria

- Record wait-until counters are visible in raw and compact probe output.
- Stats-off production paths do not perform record-wait counter atomics.
- Existing focused ownerless lock and SQL selectors pass.
- The docs record that record-wait probing is a meaningful sub-bucket but not
  the dominant remaining bulk-insert gap.

## Risks And Follow-Up

- A future single-owner bypass for record wait-until probes may save a bounded
  part of the remaining insert path, but it must prove peer-join and stale
  generation semantics before changing behavior.
- Native undo/MTR commit and page-write commit-log costs remain larger
  performance targets for parity with trunk/main.
