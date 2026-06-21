# Ownerless Single-Owner Record-Wait Skip

## Problem

The record-wait attribution slice proved that ownerless bulk inserts spend a
bounded but real amount of time in the InnoDB insert-intention availability
probe. A 16K-row production attribution run reported `16384` calls to
`ownerless_innodb_lock_wait_until_record_hook()`, `24.478 ms` total, and all
results OK. In the same sample this accounted for most of the
`row_ins_btr_lock_undo_rec_lock` ownerless delta, while larger remaining gaps
still sat in row insert, undo-report MTR commit, and page-write commit-log work.

The shared ownerless record-lock registry is still mandatory when a peer can
exist. The narrow opportunity is the continuous single-owner epoch where the
process registry proves no other ownerless process has joined or left since
this runtime opened the directory. In that case MariaDB's process-local lock
check is enough for same-process conflicts, and there is no peer record-lock
state to consult.

## Source Findings

- MariaDB base: 11.8 LTS import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/lock/lock0lock.cc`
  `lock_rec_insert_check_and_lock()` computes
  `LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION`, checks local InnoDB record-lock
  conflicts under `LockGuard`, and only then calls
  `mylite_ownerless_innodb_lock_wait_until_record_available_for_grant()` when
  ownerless hooks are enabled.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_lock_wait_until_record_available()` filters null
  transactions, zero index ids, predicate locks, missing callbacks, and
  zero transaction lock ids before invoking the MyLite record wait-until
  callback.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_lock_wait_until_record_hook()` currently normalizes the
  record resource and delegates to
  `mylite_ownerless_innodb_lock_registry_wait_until_record_available()` or the
  page-write-cycle-aware variant.
- `packages/libmylite/src/ownerless_innodb_lock_registry.cc` owns the shared
  record-lock registry slots used by product-hook tests and cross-process SQL
  waits. A single active process in the process registry does not by itself
  prove the record-lock registry is empty; same-process tests can seed an
  external ownerless record lock directly in the shared lock registry.
- `packages/libmylite/src/ownerless_process_registry.cc` increments the shared
  process-registry generation on both slot allocation and slot release.
  Therefore `active_count == 1` plus `registry_generation == owner_generation`
  proves a continuous single-owner epoch, not merely that this process is alone
  after a peer has already participated.

## Design

Add a MyLite-side fast path inside
`ownerless_innodb_lock_wait_until_record_hook()` after the normal hook context
and lock-registry validation but before taking the shared lock-registry latch.

The fast path returns `MYLITE_OWNERLESS_INNODB_LOCK_OK` only when:

- the ownerless process registry is mapped and large enough for its header;
- the current owner generation is nonzero;
- the shared process registry reports exactly one active process;
- the shared process registry generation still equals this owner's generation.
- the shared record-lock registry has no waiting entries and either has no
  active entries, has a first active entry owned by the current owner, or a
  non-mutating exact availability check proves the requested record resource is
  available.

The fast path does not run before MariaDB's local lock check. Same-process
record, gap, and insert-intention conflicts therefore remain native InnoDB
behavior. Once another ownerless process joins or leaves, generation equality
fails and the hook returns to the shared record-lock registry path. A foreign
active lock-registry entry without matching process-registry evidence also
keeps the shared registry path, preserving the product-hook coverage that
injects an external ownerless record lock inside one process.

Dedicated database perf counters report skip calls, allowed skips, unmapped
blocks, active-count blocks, and generation blocks. The existing record
wait-until call, elapsed-time, and result counters continue to report the
logical wait-until callback, including the skipped OK result.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, wire-protocol, or directory-layout
change is intended. The optimization only removes a redundant shared-registry
availability probe in a continuous single-owner epoch. Live-peer, prior-peer,
unmapped, and error cases keep the existing shared registry behavior.

## Native Storage Impact

No native InnoDB page, lock, redo, undo, checkpoint, or tablespace format
changes. The slice relies on the existing MariaDB record-lock check that has
already run before the ownerless callback.

## Performance Impact

The target cost is the previously measured `24.478 ms` per 16K-row ownerless
bulk statement in the record wait-until callback. This is a bounded hot-path
reduction, not a full ownerless write-parity claim. Row insert, undo-report MTR
commit, and page-write commit-log work remain larger performance targets.

Stats-off production overhead remains bounded to the existing hook dispatch,
the normal context validation, two process-registry loads, and cheap
record-registry header/first-active-slot checks in eligible ownerless mode.
The exact shared-registry availability check is only reached for a foreign
first active lock-registry owner. Stats atomics for the new counters execute
only when database perf stats are enabled.

Final local production probe:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=20 \
MYLITE_PERF_INSERT_ITERATIONS=32768 \
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe \
  > /tmp/mylite-perf-single-owner-record-wait-skip.log
```

Key metrics from that run:

- Ownerless bulk record wait-until: `16384` calls, `1.098 ms` total, all OK,
  with `16384` single-owner skip calls and `16384` allowed skips.
- Remaining bulk statement record wait-until: `1.098 ms` per statement, down
  from the attribution baseline's `24.478 ms`.
- Ownerless bulk rows: `72537.54 rows/s`, ordinary bulk rows:
  `91920.17 rows/s`, ratio `0.7891`.
- Remaining ownerless bulk rows: `47674.13 rows/s`, ordinary remaining bulk
  rows: `105230.81 rows/s`, ratio `0.4530`.
- Larger remaining per-statement ownerless costs are now row insert
  (`285.786 ms`), undo report (`104.685 ms`), undo-report MTR commit
  (`77.828 ms`), and page-write commit-log work (`58.187 ms`).

A corrective follow-up after CI caught the same-process external-lock product
hook case kept the same skip proof active while adding the non-mutating
record-registry availability guard. A reduced local production probe with the
same 16K-row statement shape reported `16384` allowed bulk skips,
`0.861 ms` aggregate record wait-until time per statement, and `1.723 ms` for
the remaining non-empty statement.

## Test Plan

- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_cross_process_sql_test`, and
  `mylite_embedded_ownerless_innodb_lock_hooks_test` with
  `php-embedded-prod`.
- Run focused production selectors for ownerless InnoDB lock hooks,
  single-owner multi-row insert, history WAL proof, native-support page WAL
  elision, insert FK fast path, and uncommitted peer visibility.
- Run the hook-build selectors for lock hooks, single-owner multi-row insert,
  history WAL proof, and native-support page WAL elision.
- Run stress selectors for the adjacent single-owner cases and checksum stress.
- Run a stats-enabled production performance probe and compare record
  wait-until total time and skip counters to the attribution baseline.
- Run format, production-build guard, CTest production-build guard, and
  whitespace checks.

## Acceptance Criteria

- Focused SQL coverage proves a single-owner non-empty multi-row insert has
  positive record wait-until calls and that every such call is skipped by the
  single-owner proof.
- Embedded product-hook coverage proves a same-process synthetic external
  ownerless record lock still publishes a shared waiting entry instead of being
  hidden by the single-owner process-registry proof.
- The stale-generation peer-history selector proves a later insert after peer
  join/leave has positive record wait-until calls, zero allowed skips, and a
  positive generation-block counter.
- Raw and compact performance-probe output exposes the new skip counters.
- Existing focused ownerless lock, live-reclaim, history-proof, hook-build, and
  stress selectors continue to pass.

## Risks And Follow-Up

- The proof intentionally blocks after any peer joins or leaves. This keeps the
  slice safe, but real multi-worker deployments still pay the registry probe
  after prior peer activity.
- This does not address SQL-level table-lock fault injection, broad native
  redo/checkpoint reconciliation, DDL/file-lifecycle recovery, active-reader
  pressure policy, or external MariaDB/RQG stress.
- Larger ownerless write-throughput gaps remain in native row/undo work and
  page-write publication, so this slice is only one measured hot-path reduction.
