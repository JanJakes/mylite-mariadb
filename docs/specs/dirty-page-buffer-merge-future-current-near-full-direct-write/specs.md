# Dirty Page Buffer Merge Future-Current Near-Full Direct Write

## Problem

The full future-current direct-write policy safely publishes newly allocated
full index leaves during child dirty-buffer merge, but the latest prepared
insert profile still admits `81,802` dirty `index-leaf` pages through
dirty-buffer merge pressure. Guard leaf-shape counters show a candidate subset
inside the partial future-current fallback class: `31,488` rows have only
`1-15` free slots, while the dominant `81,879` rows still have `16+` free
slots.

A broad future-current direct-write experiment regressed the prepared-insert
step to `94.432 us/op`. The next behavior slice should therefore widen the
policy only for near-full partial leaves and leave the larger free-slot class
buffered for coalescing.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage merge policy only. No upstream
  MariaDB source files or handler APIs are changed.
- `mylite_storage_commit_statement()` advances the parent statement current
  header before merging the child dirty-page buffer.
- `mylite_storage_rollback_statement()` restores the stable header, clears
  dirty and append buffers, and truncates the file to the stable statement
  header page count. Future-current pages beyond `statement->header.page_count`
  remain rollback-protected by truncation.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  direct-writes full future-current index leaves only when the parent dirty
  buffer is full, the page is below the parent current header page count, the
  page is not append-buffer resident, and the parent dirty buffer does not
  already contain the page.
- Guard leaf-shape evidence reports the partial future-current fallback rows
  by free-slot band: `3,438` with `1`, `5,753` with `2-3`, `8,913` with
  `4-7`, `13,384` with `8-15`, and `81,879` with `16+` free slots.

## Design

Add a new guard outcome:

- `future-current-header-near-full-direct-write`

When the existing future-current direct-write preconditions hold and the page
is an index leaf that is not full, direct-write it only if the leaf has at most
`15` free slots. Full leaves keep the existing
`future-current-header-direct-write` outcome. Leaves with `16+` free slots keep
the existing `future-current-header-partial-leaf` fallback outcome.

The policy continues to reject append-buffer-resident pages, parent-resident
pages, branches, pages past the current header, unsupported page sizes, and
non-full parent dirty buffers. It reuses the existing direct-write path and
checksum refresh helper, so write format and raw page publication remain
shared with the full-leaf policy.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` and other routed engine names
continue through the MyLite storage engine.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the primary `.mylite` file
plus the existing MyLite-owned journal lifecycle. Direct-written near-full
future-current leaves are newly allocated pages below the statement current
header and are discarded by rollback truncation to the stable header page
count.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds gain one guard
outcome name and focused self-test expectations.

## Tests And Verification

- Extend the future-current direct-write storage self-test to prove a near-full
  future-current leaf direct-writes, does not trigger dirty-buffer pressure,
  records the new guard outcome, records the `75-99%` fill band and `8-15`
  free-slot band, and is removed by rollback truncation with the already tested
  future-current direct-write file lifecycle.
- Preserve future-page relation coverage showing empty partial leaves still
  use `future-current-header-partial-leaf` fallback.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Future-current index leaves with `1-15` free slots direct-write under parent
  dirty-buffer pressure when the existing append-buffer, current-header, and
  parent-residency guards pass.
- Future-current leaves with `16+` free slots still use fallback replay.
- Append-buffer-resident pages, branch pages, parent-resident pages, pages past
  the current header, and non-full parent buffers still use fallback replay.
- Prepared-insert evidence shows whether the targeted near-full class reduces
  dirty-buffer pressure without repeating the broad partial-leaf regression.

## Verification Evidence

VPS verification after implementation:

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported `73.081 us/op` for the prepared insert step.
- Direct-written dirty `index-leaf` merge rows increased to `34,590`, split
  into `3,793` `future-current-header-direct-write` full leaves and `30,797`
  `future-current-header-near-full-direct-write` leaves.
- The near-full direct-write rows split by free slots as `3,431` with `1`,
  `5,728` with `2-3`, `8,846` with `4-7`, and `12,792` with `8-15`.
- `future-current-header-partial-leaf` fallback rows dropped to `51,341`, all
  with `16+` free slots.
- Dirty `index-leaf` pressure admissions from dirty-buffer merge dropped to
  `51,341`; `index-branch` pressure admissions were `288`, including `142`
  checksum-dirty branch pages.
- The final MariaDB smoke archive was `33,973,882` bytes (`32.40 MiB`).

## Risks

- Near-full leaves may still be rewritten by later child statements. The
  threshold deliberately avoids the large `16+` free-slot class, but benchmark
  evidence must decide whether the extra direct writes are profitable.
- This policy depends on the existing future-current rollback proof: newly
  allocated pages are protected by statement header truncation rather than
  dirty-page undo preimages.
