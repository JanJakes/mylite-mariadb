# Ownerless History Rseg Delta Encoding

## Problem

Ownerless insert attribution shows the current write proof still publishes one
rollback-segment `FIL_PAGE_TYPE_SYS` page and one `FIL_PAGE_UNDO_LOG` page per
hot-path autocommit insert. The undo proof page already uses the bounded
page-log delta format. The rollback-segment proof page remains a full
standalone SYS-page payload, so it is now one of the remaining page-version WAL
payload costs in the production PHPUnit-shaped ownerless path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` records both the rollback-segment
  page and undo-header page for the ownerless history proof before the commit
  mini-transaction. The proof is established only after both page-version
  publications succeed at the commit-visible LSN.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_history_proof_roles()` can identify whether a published
  page is the active rollback-segment proof page or undo proof page.
- `packages/libmylite/src/ownerless_page_log.cc` already supports durable,
  non-chained delta records for index and undo pages. Deltas carry the
  standalone base record offset, scan reconstruction verifies the full page
  checksum, and checkpoint rewrite emits retained records as standalone
  payloads.
- Broad `FIL_PAGE_TYPE_SYS` delta encoding is not safe as a performance
  shortcut because ordinary InnoDB system/allocation pages still have broader
  recovery and lifecycle roles. The hint must come from the active history
  proof path, not from page type alone.

## Design

Keep the existing history proof contract. The rollback-segment proof page is
still published as a normal ownerless native-support page version, and the undo
proof page remains published separately. This slice changes only the page-log
payload selected for an explicitly identified rollback-segment proof page.

The InnoDB page-publish hook now accepts internal publish flags. During
mini-transaction page publication, InnoDB marks only pages that match the active
rollback-segment history-proof role with
`MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_HISTORY_RSEG`. The MyLite publish hook
maps that internal flag to
`MYLITE_OWNERLESS_PAGE_LOG_APPEND_HISTORY_RSEG_DELTA`.

The page log accepts the history-rseg append option only on the selected append
call. When present and the page is an InnoDB `FIL_PAGE_TYPE_SYS` page, the
process-local delta-base cache may use a separate history-rseg delta flag for
that page identity. The durable delta shape is the same bounded, non-chained
format used for index and undo deltas:

- the delta record references a durable standalone base record offset;
- the full reconstructed page checksum remains the record integrity proof;
- checkpoint rewrite converts retained history-rseg deltas back to standalone
  records;
- base-cache identity remains scoped by page-log device, inode, offset,
  generation, page identity, page size, and delta class;
- ordinary SYS pages without the explicit history-rseg append option remain
  standalone.

The external-snapshot-lineage append path stays conservative in this slice and
does not request history-rseg delta encoding. Correctness is unchanged because
it still writes a standalone or existing generic compact payload.

## Scope And Non-Goals

In scope:

- Internal InnoDB publish flag for the active rollback-segment proof page.
- Page-log append option and record flag for hinted history-rseg deltas.
- Primitive coverage for reconstruction, latest lookup, and checkpoint rewrite.
- Production performance-probe counters for history-rseg delta records and
  payload bytes.

Out of scope:

- Eliding either history-proof page publication.
- Broad `FIL_PAGE_TYPE_SYS` delta encoding.
- Changing undo history, purge, redo, checkpoint, or native recovery rules.
- SQL-level table-lock fault injection, broader DDL/file lifecycle recovery,
  broader active-reader pressure policy, or external MariaDB/RQG stress.

## Compatibility Impact

No SQL, PHP, wire-protocol, public C API, directory-layout, or native page-format
behavior changes. The added page-log append option and InnoDB publish flag are
internal MyLite surfaces used by the embedded ownerless publish path and
primitive tests.

## Directory And Lifecycle Impact

No new files are introduced. History-rseg delta records remain ordinary records
inside `concurrency/mylite-concurrency.wal`. Checkpoint rewrite preserves the
existing invariant that retained checkpoint output does not depend on discarded
base records.

## Native Storage Impact

Native InnoDB storage and history serialization are unchanged. The rollback
segment page is still copied after the mini-transaction prepares the page for
writing and before the synchronous page-log append returns.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with the production `MinSizeRel`
  baseline after editing InnoDB source.
- Rebuild focused production targets:
  `mylite_ownerless_primitives_test`,
  `mylite_embedded_performance_probe`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run focused ownerless SQL selectors:
  `single-owner-history-wal-proof` and
  `single-owner-native-support-page-wal-elision`.
- Run a reduced stats-enabled production performance probe with page-log append,
  page-write, page-publish, and ownerless database stats enabled.
- Run production build guards, format check, and whitespace check.

## Verification Results

Local verification on 2026-06-17 used `build/mariadb-embedded` with the
production `MinSizeRel` baseline and `build/php-embedded-prod` with
first-party `Release` artifacts:

- `tools/mariadb-embedded-build build` rebuilt the embedded MariaDB archive.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- Focused production selectors
  `libmylite.ownerless-single-owner-history-wal-proof` and
  `libmylite.ownerless-single-owner-native-support-page-wal-elision` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, followed by the hook
  selectors `libmylite.ownerless-single-owner-history-wal-proof`,
  `libmylite.ownerless-single-owner-native-support-page-wal-elision`, and
  `libmylite.ownerless-history-proof-publish-failure-fallback`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`,
  `MYLITE_PERF_OWNERLESS_PAGE_LOG_APPEND_STATS=1`,
  `MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1` reported `3.008` ownerless
  autocommit page versions per insert, `1.000` published rollback-segment proof
  page per insert, `1.000` published undo proof page per insert, `0.748`
  history-rseg delta records per insert, `23.398` history-rseg delta payload
  bytes per insert, `0.752` undo-delta records per insert, total page-log
  payload at `1082.168` bytes per insert, page-log append at `0.052 ms/insert`,
  and page-log encode at `0.028 ms/insert`. The preceding same-shape
  stats-enabled sample before this slice reported total page-log payload at
  `1112.772` bytes per insert and no history-rseg delta counters.
- A reduced stats-off production probe with the same iteration counts reported
  ordinary autocommit inserts at `2886.07 ops/s`, ownerless autocommit inserts
  at `1556.63 ops/s` (`0.5394` ratio), ordinary explicit-transaction inserts
  at `3808.65 ops/s`, ownerless explicit-transaction inserts at
  `1891.57 ops/s` (`0.4967` ratio), and ownerless active-runtime reconnect at
  `0.865 ms`.
- Ownerless stress coverage rebuilt under `ownerless-stress`. The general
  stress, DDL stress, temporary-table stress rerun, transaction stress,
  random-transaction stress, foreign-key graph stress, child-failure cleanup,
  active-reader pressure, expanding-page pressure, BLOB pressure, and
  compressed-BLOB pressure tests passed. The checksum stress test failed twice
  with `MYLITE_BUSY` / `ownerless table write statement lock is busy`; this
  statement-lock wait/fairness issue was not addressed by the history-rseg
  payload slice and remains unresolved here.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Unhinted SYS pages remain standalone and record zero history-rseg delta stats.
- Hinted rollback-segment SYS pages can encode as history-rseg delta records
  after a standalone base is available.
- Direct record read and latest lookup reconstruct the hinted page exactly.
- Checkpointing a WAL containing a retained history-rseg delta rewrites it as a
  standalone record.
- Focused SQL proof selectors continue to prove the existing two-page native
  history proof and fallback behavior.

## Risks And Unresolved Questions

- This reduces payload size; it does not replace the two-page history proof.
  A smaller proof still needs separate design and recovery evidence.
- The performance benefit depends on the hot path repeatedly touching the same
  rollback-segment page. Broader workloads may still publish standalone SYS
  pages outside this hinted proof path.
- External-snapshot-lineage appends remain conservative until there is focused
  evidence that the retained external-lineage semantics should use the same
  payload optimization.
