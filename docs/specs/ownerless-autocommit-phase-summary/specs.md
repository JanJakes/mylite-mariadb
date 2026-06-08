# Ownerless Autocommit Phase Summary

## Problem Statement

Production CI now runs timing-sensitive jobs with optimized build presets and
the embedded performance probe emits compact top-level summary keys. The
remaining ownerless write-performance question is deeper: one-row autocommit
insert cost is concentrated in ownerless page-version publication, InnoDB
commit write-history, and row-insert internals. The detailed stats already
exist, but CI log comparison still requires manually dividing totals by the
current insert iteration count.

This slice adds derived per-insert summary keys for the ownerless autocommit
insert phase when `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` routes
  `trx_commit_for_mysql()` into `trx_t::commit()`,
  `trx_t::commit_persist()`, `trx_t::write_serialisation_history()`, and
  `trx_t::commit_in_memory()`.
- `mariadb/storage/innobase/row/row0mysql.cc:row_insert_for_mysql()` drives
  the InnoDB row insert path.
- `mariadb/storage/innobase/row/row0ins.cc` contains the lower
  `row_ins_*` index insertion path, including clustered low-level insertion.
- `docs/specs/ownerless-innodb-deep-autocommit-profile/specs.md` records that
  the reduced production ownerless autocommit sample is dominated by
  `trx_t::write_serialisation_history()` and commit-MTR page publication, with
  row insertion as the next major block.
- `packages/libmylite/tests/embedded_performance_probe.c` already reads and
  prints detailed page-publish, page-log append, page-write, database, InnoDB
  handler, and deep InnoDB stats when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Design

Add a diagnostic-only helper in the embedded performance probe that reads the
same stats arrays after the ownerless autocommit insert phase and emits
`mylite_perf_summary_ownerless_autocommit_*` keys:

- page versions per insert;
- native-support page versions per insert;
- snapshot-boundary page versions per insert;
- native-support page ratio;
- page-publish hook total, append, and index time per insert;
- page-log append time per insert;
- page-write refresh and publish time per insert;
- commit-MTR publish time per insert;
- InnoDB write-history time per insert;
- InnoDB ownerless commit-visibility time per insert;
- row-insert and clustered optimistic B-tree insert time per insert.

Emit these keys only when the existing detailed stats mode is enabled. The
normal stats-off production probe path remains unchanged.

## Scope

In scope:

- Derived ownerless autocommit insert phase summaries in the production
  embedded performance probe.
- Documentation updates for CI/performance interpretation.
- Focused production-build verification.

Out of scope:

- Engine optimization.
- Redo/checkpoint policy changes.
- New public API.
- WordPress PHPUnit harness changes.

## Compatibility Impact

No SQL, C API, PHP API, storage-engine, locking, recovery, checkpoint, or
directory-lifecycle behavior changes. This slice only emits additional
diagnostic lines from the test/performance probe.

## Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe still creates and
removes its temporary database directory.

## Native Storage Impact

No native storage format or InnoDB behavior changes.

## Public API Impact

No public API changes.

## Build And Binary-Size Impact

No library binary-size impact. The test/probe binary gains a small helper that
prints derived stats when detailed stats are enabled.

## Test Plan

- Build the production embedded performance probe with
  `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`.
- Run a reduced production stats-enabled probe and confirm the new
  `mylite_perf_summary_ownerless_autocommit_*` keys are emitted.
- Run focused ownerless primitive CTest coverage under `php-embedded-prod`.
- Run `cmake --build --preset format`.
- Rebuild the performance probe after formatting.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Stats-off probe output remains unchanged except for existing top-level
  summaries.
- Stats-on ownerless autocommit output includes per-insert phase summaries.
- The summaries are derived from existing detailed counters and do not read
  clocks or mutate engine state.
- Production verification passes.

## Verification Results

Local verification on 2026-06-08 used the production
`build/php-embedded-prod` performance probe.

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`: passed.
- Reduced stats-enabled production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=5`,
  `MYLITE_PERF_INSERT_ITERATIONS=5`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`:
  passed and emitted the new summary keys. The sample reported
  `mylite_perf_summary_ownerless_autocommit_page_versions_per_insert=7.000`,
  `mylite_perf_summary_ownerless_autocommit_native_support_pages_per_insert=6.000`,
  `mylite_perf_summary_ownerless_autocommit_native_support_page_ratio=0.8571`,
  `mylite_perf_summary_ownerless_autocommit_page_publish_hook_ms_per_insert=0.285`,
  `mylite_perf_summary_ownerless_autocommit_page_log_append_ms_per_insert=0.230`,
  `mylite_perf_summary_ownerless_autocommit_commit_mtr_publish_ms_per_insert=0.355`,
  `mylite_perf_summary_ownerless_autocommit_write_history_ms_per_insert=5.087`,
  `mylite_perf_summary_ownerless_autocommit_ownerless_visibility_ms_per_insert=1.250`,
  and `mylite_perf_summary_ownerless_autocommit_row_insert_ms_per_insert=1.214`.
- Reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=2`, and
  `MYLITE_PERF_INSERT_ITERATIONS=2`: passed, retained the existing
  `mylite_perf_summary_ownerless_insert_autocommit_ops_per_second` key, and
  emitted no `mylite_perf_summary_ownerless_autocommit_*` phase keys.
- `ctest --preset php-embedded-prod -R
  'libmylite\\.ownerless-primitives$' --output-on-failure`: passed, 1/1
  test, 2.82s.
- `cmake --build --preset format` passed before the production build.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks And Unresolved Questions

- Stats-on runs are attribution evidence and can perturb timings. Use
  stats-off throughput for production performance comparison.
- Per-insert summaries make CI comparison easier, but optimization still
  requires a separate correctness slice for any redo/checkpoint or page
  publication change.
