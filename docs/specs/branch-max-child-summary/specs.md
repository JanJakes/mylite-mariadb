# Branch Max-Child Summary

## Problem

Prepared insert profiling on the VPS after the embedded syscall fixes showed
remaining step time under maintained branch insert work. A high-frequency
`gprofng` run over
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
reported visible time in `write_branch_index_root_inserts()`,
`redistribute_branch_index_leaf_range_entry()`, checksum refresh, and
`index_branch_max_child_page_id()`.

`index_branch_max_child_page_id()` currently scans every decoded branch child
cell whenever branch-tail overlay logic needs the greatest child page id. The
branch decoder has already walked the child cells for corruption and ordering
checks, so later same-struct consumers should not rescan the payload just to
recover the same maximum child page id.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party storage branch insert planning and writing lives in
  `packages/mylite-storage/src/storage.c`, including
  `plan_branch_index_root_insert()`, `write_branch_index_root_inserts()`, and
  branch-tail overlay cache helpers.
- `decode_index_branch_page()` validates every child page id and child fence in
  a branch page before returning a `mylite_storage_index_branch_page` view.
- `index_branch_tail_has_live_overlay()` calls
  `index_branch_max_child_page_id()` before probing or storing branch-tail
  overlay cache entries.

## Design

Add a decoded summary field to `mylite_storage_index_branch_page` for the
maximum child page id. `decode_index_branch_page()` fills it while it already
walks child cells. `index_branch_max_child_page_id()` then returns the summary
when present and keeps the existing payload scan as a fallback for synthetic
test structs or older internal callers that do not fill the summary.

This preserves the current corruption checks because only the decoder writes
the summary for durable branch pages. Refresh helpers that mutate branch pages
decode the updated bytes before passing the branch view onward, so the summary
tracks the same page image as the payload pointer.

## Compatibility Impact

No SQL-visible behavior changes. Routed `ENGINE=InnoDB`, explicit `MYLITE`, and
other supported engine aliases still use the same MyLite handler and storage
paths. The change only removes redundant process-local scanning after a branch
page has already been decoded.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The summary is transient decoded state and is never written to the
`.mylite` file.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public API, durable file-format, license, or dependency change. Binary-size
impact is limited to one in-memory field and a small fallback branch.

## Test And Verification Plan

- Add a storage test hook that proves `index_branch_max_child_page_id()` can use
  a decoded summary without touching child payload bytes.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Decoded branch page views retain their maximum child page id.
- Branch-tail overlay checks use the retained summary instead of rescanning the
  branch child payload.
- Synthetic branch views without a summary keep the existing payload-scan
  fallback.
- Existing storage, embedded storage-engine, and performance smoke checks pass.

## Verification

Run on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format reported no modified files.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; rebuilt `libmysqld/libmariadbd.a` at 32.40 MiB with
  `PLUGIN_MYLITE_SE=STATIC`.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  1/1 tests in 342.31 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests in 346.19 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step measured 78.803 us/op, bind measured 0.841 us/op,
  reset measured 0.152 us/op, and commit measured 62.133 ms. Storage counters
  showed zero branch leaf-range plan reads, zero branch refold reads, zero
  branch tail overlay scans, zero branch tail overlay scan reads, and two
  packed index tail-append missing-page blockers.

## Risks

The summary must stay tied to the decoded page image. The fallback path avoids
making synthetic or partially initialized structs unsafe, and branch mutation
helpers already decode updated page bytes before consumers use the branch view.
