# Indexed Row Validated Mark Only

## Problem Statement

Indexed-row payload lookup marks an exact-index hit as live before reading the
row payload, then marks the same row as validated after the payload read
succeeds. The validated marker already records both live and validated row ids,
so the successful hot path pays for two live-row cache updates.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns `find_indexed_row_payload()` and the
  statement live-row cache helpers in `packages/mylite-storage/src/storage.c`.
- `mark_active_validated_live_row_in_statement()` adds the row id to the live
  cache before adding it to the validated cache.
- A failed payload read in this path is returned to the caller and does not
  need a performance-only live-row cache mark for correctness.

## Proposed Design

- Remove the pre-read `mark_active_live_row_in_statement()` call from
  `find_indexed_row_payload()`.
- Keep the post-read `mark_active_validated_live_row_in_statement()` call as
  the single successful-path live-row cache update.

## Affected Subsystems

- Indexed-row storage read hot path.
- Statement-owned live-row validation cache.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. Successful lookups still mark the row as live and validated.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

No new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Successful indexed-row payload reads perform one validated live-row cache
  update instead of a live update followed by a validated update.
- Storage and embedded tests pass.
- Prepared-update profiling reduces live-row cache marking samples or shows the
  next bottleneck clearly.

## Risks And Open Questions

- A failed payload read no longer leaves a live-row performance cache mark.
  That cache is statement-local optimization state, not durable behavior.
