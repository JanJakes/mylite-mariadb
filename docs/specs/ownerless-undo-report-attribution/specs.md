# Ownerless Undo Report Attribution

## Problem

The production CI run for `970aa207` confirmed that the ownerless insert
performance delta has moved below the optimistic B-tree lock/undo aggregate.
The stats-enabled embedded attribution probe reported:

- ownerless-minus-ordinary clustered-low total: `0.248 ms/insert`,
- row-level clustered-low MTR commit: `0.129 ms/insert`,
- clustered optimistic B-tree total: `0.116 ms/insert`,
- optimistic B-tree lock/undo: `0.115 ms/insert`,
- record lock checking: `0.001 ms/insert`,
- `trx_undo_report_row_operation()`: `0.114 ms/insert`, and
- no lock/undo skip, lock-wait, `DB_FAIL`, or other-error counts.

`trx_undo_report_row_operation()` is itself a compound InnoDB path. Before
optimizing it, MyLite needs production-build evidence that separates undo
assignment, cached-undo reuse/header rewrite, fresh undo creation, undo-record
encoding, mini-transaction commit/publication, page extension, success
bookkeeping, and error exits.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc` implements
  `trx_undo_report_row_operation()`, which:
  - records table-modification state in `trx->mod_tables`,
  - handles bulk-insert and online-DDL flags,
  - starts an `mtr_t`,
  - assigns persistent undo through `trx_undo_assign_low<false>()`,
  - writes insert undo records through `trx_undo_page_report_insert()` for
    simple inserts,
  - commits the undo-report mini-transaction,
  - updates the in-memory undo top pointers and transaction undo number, and
  - returns a rollback pointer for non-bulk row operations.
- `mariadb/storage/innobase/trx/trx0undo.cc` implements the lower undo paths:
  - `trx_undo_reuse_cached()` rewrites a cached undo header and reinitializes
    a process-local undo object,
  - `trx_undo_create()` creates a fresh undo segment/header, and
  - `trx_undo_assign_low<false>()` chooses existing, cached, or fresh undo.
- Prior ownerless undo-cache work already proved cached undo reuse is safe only
  under MyLite's continuous single-owner proof. This slice does not broaden
  that proof or change cached-undo behavior.

## Design

Add stats-gated deep InnoDB counters for:

- undo-report prelude,
- persistent and temporary undo assignment,
- insert undo-record page reporting,
- update/delete undo-record page reporting,
- undo-report mini-transaction commits,
- success bookkeeping,
- add-page attempts and successes,
- record-too-big and out-of-space/error exits,
- cached-undo reuse list lookup,
- cached-undo page get/latch,
- cached-undo header rewrite and memory reinitialization,
- fresh undo create rollback-segment block lookup,
- fresh undo segment creation,
- fresh undo header/object initialization, and
- insert undo-record header, unique-field loop, virtual-column section, and
  final append.

Emit raw `mylite_perf_*_innodb_deep_trx_undo_report_*`,
`*_trx_undo_reuse_cached_*`, `*_trx_undo_create_*`, and
`*_trx_undo_page_report_insert_*` values, plus compact ordinary, ownerless, and
ownerless-minus-ordinary autocommit summaries when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

Keep all timers behind the existing deep-counter stats gate. Stats-off
production probes must not emit the new keys and must avoid timestamp reads for
the new subphases.

## Affected Subsystems

- Upstream-derived InnoDB undo reporting:
  `mariadb/storage/innobase/trx/trx0rec.cc`.
- Upstream-derived InnoDB undo assignment helpers:
  `mariadb/storage/innobase/trx/trx0undo.cc`.
- MyLite deep performance counter enum:
  `mariadb/storage/innobase/include/mylite_ownerless_innodb_deep_perf.h`.
- Embedded production performance probe:
  `packages/libmylite/tests/embedded_performance_probe.c`.
- Ownerless performance specs and compatibility notes.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, locking, metadata, storage format, recovery, or
directory-lifecycle behavior changes. The slice adds diagnostic counters only.

## Database Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe continues to create
and remove temporary MyLite directories.

## Public API Impact

No public API changes.

## Native Storage Impact

No undo, redo, rollback-segment, purge, page, checkpoint, or tablespace format
changes. The instrumentation observes existing MariaDB/InnoDB paths and must
preserve control flow and return values.

## Build And Performance Impact

The new counters use the existing stats-enabled diagnostic path. Stats-off
production execution should pay only the existing fast atomic check used by the
deep-performance helpers.

## Implementation Notes

The slice adds deep counters to:

- `trx_undo_report_row_operation()` for prelude, assignment, page-report,
  MTR-commit, success-bookkeeping, page-extension, and error paths,
- `trx_undo_page_report_insert()` for undo-record header, unique-field,
  virtual-column, append, success, and no-space paths,
- `trx_undo_reuse_cached()` for lookup, page get, header rewrite, memory
  reinitialization, dict writes, miss, page-get failure, and success counts,
  and
- `trx_undo_create()` for rollback-segment lookup, segment creation, header
  creation, memory object creation, dict writes, success, and failure counts.

The probe mirrors the enum and emits compact
`mylite_perf_summary_*_autocommit_trx_undo_*` comparisons only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Verification Evidence

Local production verification on 2026-06-11:

- `tools/mariadb-embedded-build build` rebuilt the MariaDB embedded archive
  under `MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  rebuilt the probe and focused SQL test under the production preset.
- A reduced 100-row stats-enabled production probe reported:
  - ownerless-minus-ordinary clustered-low total `0.201 ms/insert`,
  - clustered-low row-level MTR commit `0.086 ms/insert`,
  - clustered optimistic B-tree `0.107 ms/insert`,
  - optimistic lock/undo `0.105 ms/insert`,
  - undo report `0.103 ms/insert`,
  - undo-report MTR commit `0.113 ms/insert`,
  - undo-report persistent assignment `-0.011 ms/insert`,
  - ownerless cached-undo page get `0.005 ms/insert`,
  - ownerless cached-undo header rewrite `0.002 ms/insert`,
  - fresh undo segment creation `-0.018 ms/insert`,
  - near-zero page-record encoding and success-bookkeeping deltas, and
  - zero assign, record-size, out-of-space, and other-error counts.
- The same raw stats-enabled sample reported ownerless autocommit cached-undo
  attempts at `1.000` per insert, hits at `0.810` per insert, fresh undo
  creates at `0.190` per insert, and undo-report successes at `1.000` per
  insert.
- A reduced 500-row stats-off production probe emitted no
  `trx_undo_report`, `trx_undo_reuse_cached`, `trx_undo_create`, or
  `trx_undo_page_report_insert` keys.
- `tools/check-ci-production-builds`, `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure`,
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-primitives|libmylite\.ownerless-single-owner-history-wal-proof|libmylite\.ownerless-single-owner-native-support-page-wal-elision'
  --output-on-failure`, explicit ownerless transaction/lock hook tests,
  ownerless independent-table and transaction stress trace selectors,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.
- The full `ctest --preset ownerless-stress --output-on-failure` run passed
  independent-table, DDL, temporary-table, and transaction stress, then timed
  out in `libmylite.ownerless-cross-process-checksum-stress` at `900.10s`
  with the existing InnoDB fatal semaphore wait on `dict_sys.latch`. A direct
  isolated `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=160
  mylite_ownerless_cross_process_sql_test checksum-stress` rerun reproduced
  the same fatal wait with stats disabled. No ownerless processes or
  `/tmp/mylite-ownerless-*` directories remained after cleanup. This matches
  the checksum-stress debt already recorded in
  `docs/specs/ownerless-native-support-publish-attribution/specs.md` and is
  not evidence that the stats-gated undo-report counters changed runtime
  behavior.

## Test And Verification Plan

- Rebuild the production `MinSizeRel` MariaDB embedded archive after editing
  InnoDB sources.
- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production attribution probe and confirm the new
  undo-report summary keys are present.
- Run a reduced stats-off production probe and confirm the new undo-report
  attribution keys are absent.
- Run focused ownerless primitive/history tests and bounded hook/stress
  selectors.
- Run `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`,
  production format check, and `git diff --check`.

## Acceptance Criteria

- The deep-counter enum stays aligned between MariaDB and the C performance
  probe mirror.
- Stats-enabled production attribution splits the undo-report bucket into
  assignment, lower cached/fresh undo work, page reporting, MTR commit,
  bookkeeping, page extension, and error classes.
- Stats-off production throughput output remains unchanged.
- The implementation preserves upstream undo-report and undo-assignment control
  flow and return values.
- Docs continue to avoid claiming ownerless concurrency completion or broad
  cached-undo safety.

## Risks And Unresolved Questions

- This slice identifies the dominant subphase; it does not optimize it.
- If the undo-report mini-transaction commit dominates, the next optimization
  belongs near ownerless page-publication or native-support proofing.
- If cached-undo reuse/header rewrite dominates, any optimization must stay
  within the continuous single-owner proof.
- If fresh undo creation dominates, the next optimization must improve reuse
  hit rate without making process-local cached undo visible across peers.
