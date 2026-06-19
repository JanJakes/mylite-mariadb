# Ownerless Redo Active Reservation Count

## Problem

The ownerless redo written/leave fusion slice reduced one progress-latch pass
for production top-level redo ranges, but the remaining redo-state leave path
still checked whether any redo reservation was active by scanning the 64-slot
reservation table. The embedded attribution probe continues to show thousands
of redo enter/reserve/leave operations for bulk inserts, so repeated O(64)
bookkeeping remains a visible target even when it is not the dominant ownerless
write cost.

## Design

Store a maintained active-reservation counter in the unused bytes between the
redo-state visible-generation field and the progress latch:

- offset `88` stores a `uint32_t` active reservation count;
- redo-state shared-memory segment metadata moves from version `8` to `9`;
- reservation creates increment the counter while holding the progress latch;
- write completion decrements the counter when it clears an active reservation
  slot;
- entry-refcount slots continue to use the same slot table but do not affect
  the reservation counter;
- snapshots and leave-time latest-LSN advancement read the counter directly
  instead of scanning all reservation slots.

The counter is an internal shared-state acceleration. It does not change redo
reservation ordering, completed-range draining, latest/visible LSN semantics,
checkpoint persistence, page-version WAL records, SQL behavior, or public C
API behavior.

## Compatibility Impact

The shared-memory segment version bump causes a runtime using older redo-state
metadata to be rebuilt rather than interpreted with the new counter layout. The
durable database directory format, checkpoint files, page log, and native
InnoDB files are unchanged.

## Verification Plan

- Build production primitive, embedded open/close, ownerless SQL, and
  performance probe targets.
- Run `libmylite.ownerless-primitives` to prove active reservation counts still
  track reserve/complete/cleanup behavior.
- Run `libmylite.embedded-open-close` to prove the shared-memory redo segment
  descriptor reports version `9`.
- Run a reduced stats-enabled ownerless bulk attribution probe to ensure page
  publication and redo counters remain stable.
- Run a larger stats-off ownerless bulk probe to capture the production timing
  band.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_open_close_test
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure`
- `ctest --preset embedded-prod -R '^libmylite\.embedded-open-close$'
  --output-on-failure`
- `ctest --preset embedded-prod -R
  'libmylite\.(ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|ownerless-cross-process-sql\.(0|1|2))$'
  --output-on-failure`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-written-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-latest-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-latest-checkpoint-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-gap-blocks-writer`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  stress`
- `ctest --preset embedded-prod -R
  '^tools\.ownerless-transaction-stress-trace$|^tools\.ownerless-active-reader-pressure-trace$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced stats-enabled probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=3000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It preserved the expected publication volume for 30 bulk statements:

- `2961` page-publish candidates;
- `60` published page versions;
- `2901` native-support elisions;
- `5835` redo enter/reserve/leave calls.

The production stats-off timing probe used 30000 rows and 100 rows per
statement, reporting:

- ordinary bulk: `95543.15 rows/s`;
- ownerless bulk: `21802.61 rows/s`;
- ownerless/ordinary bulk ratio: `0.2282`.

This verifies the counter path in the current production timing band, but the
remaining ownerless write gap is still dominated by broader native execution,
page-version publication, and redo/checkpoint proof work.
