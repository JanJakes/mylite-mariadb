# Ownerless Delta Eligibility Reuse

## Problem Statement

The ownerless page-log append path classifies each page image to decide whether
it can use the bounded non-chained delta cache. The same append then classified
the same page again after writing the record so it could update the
process-local delta base table.

Current production attribution keeps pointing at native commit/page-publication
as the larger remaining performance target, but page-log append is still a
first-party hot path. This slice removes the duplicated classification without
changing page-version WAL bytes, delta eligibility, checkpoint rewrite,
recovery, or SQL behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` passes a stable committed page image
  to the MyLite ownerless page-log append hook after installing the commit LSN.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  selects page-log payload encoding, optionally looks up a process-local delta
  base, writes the page-version record, then updates that process-local base
  cache after a successful append.
- `page_delta_flag_for_page()` is the local eligibility gate for
  `FIL_PAGE_INDEX`, `FIL_PAGE_UNDO_LOG`, and explicitly hinted history-rseg
  `FIL_PAGE_TYPE_SYS` deltas. It is a pure classification step over the same
  immutable page image used by the append.
- `index_delta_base_snapshot()` already accepts the concrete delta flag. The
  removed wrapper only recomputed that flag before calling it.

## Design

Classify the page once at the start of `append_record_at_locked()` and keep the
result in local `page_delta_eligible` and `page_delta_flag` variables.

Use that cached flag for:

- the delta-base snapshot lookup before encoding;
- the post-append delta-base note/update after the record header is durable.

If the page is not delta-eligible, skip the post-append base-note helper
entirely. For eligible pages, preserve the existing base-cache lock, slot
validation, standalone-base refresh, delta-run counter update, and allocation
failure behavior.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli API, wire-protocol behavior, storage
format, page-log record format, checkpoint rule, or recovery behavior changes.
Existing records remain readable and new records use the same flags and payload
bytes as before.

## Directory And Lifecycle Impact

No durable file, shared-memory field, startup behavior, close behavior, cleanup
rule, or process ownership rule changes.

## Native Storage Impact

Native InnoDB page images, redo, undo, checkpoints, and crash recovery are
unchanged. The slice only removes duplicated first-party page classification
around the same append operation.

## Build And Performance Impact

The expected effect is a small CPU reduction in page-log append work, most
visible in the stats-enabled `delta_base_note` and total append timings. It is
not expected to change append counts, payload bytes, delta counts, page-version
publication counts, or ownerless durability behavior.

The higher-impact performance targets remain native commit/page-publication
proof volume and broader redo/checkpoint reconciliation.

## Test And Verification Plan

- Build the production ownerless primitive test, cross-process SQL test, and
  embedded performance probe.
- Run `libmylite.ownerless-primitives` under `php-embedded-prod` to cover
  byte-exact index, undo, history-rseg, sparse, checkpoint, and replay payloads.
- Run focused ownerless SQL selectors for the history WAL proof,
  native-support page WAL elision, visible-fast insert, and uncommitted peer
  visibility.
- Run a reduced stats-enabled production embedded performance probe and confirm
  append counts, payload bytes, and delta counts remain stable.
- Run the production-build audit, format check, and whitespace check.

## Verification Results

Local verification on 2026-06-17 used the existing production
`php-embedded-prod` first-party Release artifacts and the existing `MinSizeRel`
MariaDB embedded archive:

- `cmake --build --preset php-embedded-prod` passed, including the embedded
  hook test target that had failed in CI from an outdated test callback
  signature.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed after formatting.
- Focused production CTest selectors passed:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-ownerless-innodb-lock-hooks$|^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure`.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. The ownerless bulk
  autocommit phase reported `155` page-log append calls, `58674` payload
  bytes, `94` index-delta records, `2` undo-delta records, `1`
  history-rseg-delta record, and `0.743 ms` in delta-base note-update time.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Page-log primitive coverage passes with unchanged delta record behavior.
- Focused ownerless SQL coverage passes.
- Reduced production attribution still reports stable page-log append counts,
  payload bytes, and delta record counts.
- No native-storage, directory-lifecycle, checkpoint, or public API semantics
  change.

## Risks And Follow-Up

This is a bounded first-party hot-path cleanup, not the final ownerless
performance answer. Native commit/page-publication cost, remaining proof-page
volume, group-commit batching, and broader redo/checkpoint recovery remain the
larger tasks.
