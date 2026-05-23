# Payload-Only Buffered Update Undo

## Problem

Same-size active buffered update rewrites mutate only row payload bytes, and for
single changed-index rewrites mutate only row payload plus fixed key bytes. The
current undo capture starts at each page checksum field, so hot prepared update
loops copy checksum and metadata bytes that are not modified by the rewrite.

The current `prepared-row-only-update-components` sample still shows
`rewrite_active_row_only_update_page()` and its payload copy on the hot path.
This slice keeps the existing rollback model but narrows the captured byte
ranges for same-size rewrites.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `rewrite_active_row_only_update_page()` uses the same-size fast path by
  copying only `row_size` bytes at `ROW_PAYLOAD_OFFSET` and setting the
  buffered checksum-dirty flag.
- `rewrite_active_single_index_update_page()` uses the same-size fast path by
  copying row payload bytes and key bytes while leaving row metadata and index
  metadata unchanged.
- Offset-based buffered undo restoration can conservatively leave the checksum
  dirty when the checksum field is not part of the saved range, so it can
  safely restore only the modified payload/key bytes.

## Scope

- Capture row-only same-size undo from `ROW_PAYLOAD_OFFSET` for `row_size`
  bytes.
- Capture single-index same-size row undo from `ROW_PAYLOAD_OFFSET` and index
  undo from `INDEX_KEY_OFFSET`.
- Preserve the existing broader undo range for zero-length payload/key edge
  cases and size-changing rewrites.
- Keep page format, journal format, checksum refresh, and rollback semantics
  unchanged.

## Non-Goals

- No change to MariaDB prepared statement execution, table opening, or DML
  rebind work.
- No change to dirty-page journal protection or durable page formats.
- No attempt to avoid payload copies themselves.

## Compatibility Impact

No SQL, storage-engine routing, C API, metadata, or file-format behavior
changes. Rollback restores the same logical row and index state as before.

## Test Plan

- Run the storage unit test. Existing active same-size rollback coverage checks
  row-only and single-index savepoint rollback after checksum-dirty state has
  already been refreshed.
- Run the storage-smoke test preset.
- Run `prepared-row-only-update-components` as local before/after performance
  evidence.
- Run `git diff --check` and clang-format diff checks for the touched C file.

## Acceptance Criteria

- Same-size row-only and single-index active rewrites capture payload/key undo
  ranges rather than checksum-to-payload ranges.
- Existing active rollback and storage-smoke coverage passes.
- Prepared row-only update timing does not regress.

## Verification Results

Verified on 2026-05-23:

```sh
cmake --build --preset storage-smoke-dev --target mylite_storage_test
build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline
ctest --preset storage-smoke-dev --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 10000 1000000
```

The local performance sample measured:

- prepared row-only update bind: `0.022 us/op`
- prepared row-only update step: `1.672 us/op`
- prepared row-only update reset: `0.022 us/op`

## Risks And Follow-Ups

- This only removes unnecessary undo bytes. The larger prepared-DML wall remains
  MariaDB table-open, `prepare_inner()`, `JOIN::prepare()`, and handler
  execution setup.
