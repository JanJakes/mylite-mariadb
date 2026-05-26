# Maintained Index Row-Only Overflow Update

## Goal

Keep prepared row-only updates correct after a maintained index root overflows
from a sealed single-page root into a tail or branch-backed root.

`UPDATE ... WHERE primary_key = ?` over a routed `ENGINE=InnoDB` table should
still update exactly the matching MyLite row when the updated fields do not
change any index entry.

## Non-Goals

- No new B-tree split, merge, or compaction behavior.
- No physical branch-leaf retarget optimization for row-only updates.
- No change to storage-engine routing policy or public `libmylite` APIs.
- No new file-format version or persistent sidecar file.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::execute_mylite_prepared_direct_update()`
  calls the handler direct-update path for the prepared exact-key update shape.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` uses
  `direct_update_row_preserving_index_entries()` when the update cannot change
  index fields.
- `packages/mylite-storage/src/storage.c::mylite_storage_update_row_preserving_index_entries_in_statement()`
  routes that handler call into `update_row_with_index_entries_for_context()`.
- `packages/mylite-storage/src/storage.c::plan_maintained_index_root_retargets()`
  physically retargets sealed single-page maintained roots, but currently
  returns `MYLITE_STORAGE_UNSUPPORTED` when the root has an overflow tail or is
  a branch page.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()` and
  related scan helpers already combine root or branch entries with later
  row-state pages when a live tail exists, so an appended row-state replacement
  is sufficient for overflow and branch-backed roots.

## Compatibility Impact

This fixes a routed MySQL/MariaDB DML compatibility regression. Application
DDL such as `ENGINE=InnoDB` still resolves to MyLite under the active
file-backed session, and row-only prepared updates should not silently miss rows
after the maintained primary index root reaches capacity.

`docs/COMPATIBILITY.md` does not need a new claim; this remains within the
existing routed DML and row/index storage coverage.

## Design

Keep the existing physical retarget plan for sealed single-page maintained root
pages, because those readers can continue using the static root without scanning
later row-state pages.

For maintained roots with an overflow tail and for branch roots, planning should
not fail the update. Those layouts already force exact and entryset readers onto
the live-tail path after a row-state page is appended. The preserving-index
update should therefore write the replacement row and row-state page, then rely
on the existing live-tail row-state overlay to retarget the index entry.

This keeps handler behavior thin: the MariaDB-facing direct update path still
asks storage for a preserving-index update; storage decides whether to perform
a physical root retarget or use the row-state overlay.

## File Lifecycle

The update appends normal MyLite row and row-state pages inside the primary
`.mylite` file and uses the existing statement or transaction journal. No new
companions or durable sidecars are introduced.

## Embedded Lifecycle And API

No public API changes. The fix applies under the existing embedded active
statement or transaction checkpoint used by prepared updates.

## Build, Size, And Dependencies

No dependency, license, or build-profile changes. The code change is limited to
first-party storage planner behavior plus tests and documentation.

## Test Plan

- Add storage coverage that fills a maintained index root past single-page
  capacity, verifies exact lookup for root and overflow rows, then performs
  preserving-index updates and verifies key lookup returns the replacement row.
- Run the storage unit build and `mylite-storage` CTest target.
- Run the prepared row-only update performance component at the overflow
  threshold and above it.
- Run formatting and whitespace checks for the touched files.

## Acceptance Criteria

- `mylite_storage_update_row_preserving_index_entries_in_statement()` succeeds
  for rows indexed by a maintained root after overflow.
- Exact key lookups return the replacement row id and payload after the update.
- Prepared row-only update benchmark checksums are correct at and above the
  former overflow failure threshold.
- Temporary tracing is removed before commit.

## Risks And Open Questions

- Overflow and branch-backed roots use the existing live-tail overlay instead
  of a physical branch retarget. That is correct but can still leave performance
  work for future navigable branch maintenance slices.

## Verification Results

Passed:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
tools/mylite-perf-baseline --phase=prepared-row-only-update-components 335 1
tools/mylite-perf-baseline --phase=prepared-row-only-update-components 400 400
```
