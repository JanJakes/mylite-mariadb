# Row Update Append Buffer Scope

## Problem

The row-update path already opens a storage update scope and resolves the
nearest active statement. The active buffered rewrite helper still starts by
rediscovering both the same active statement and the outer append-buffer owner
from the `FILE *`. Prepared point updates call this helper for every row
mutation inside a transaction, so the repeated statement-chain walk remains in
the hot path after the previous active cache threading work.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes.
- `open_existing_file_for_update_scope()` returns the nearest active write
  statement for the current file when row DML runs inside an active checkpoint.
- `active_statement_and_append_buffer_for_file()` walks the active statement
  chain to find the nearest statement and the outermost append-buffer statement
  for the same file.
- `rewrite_active_update_pages()` only needs those resolved statements; after
  startup it does not otherwise use the `FILE *`.
- The hot benchmark remains `tools/mylite-perf-baseline
  --phase=prepared-updates 1000 1000000`.

## Design

- Add a small helper that derives the outer append-buffer statement by walking
  parents from an already-resolved active statement, preserving same-file and
  active-owner checks.
- Resolve that append-buffer statement once in `update_row_with_index_entries()`
  next to the existing active statement.
- Change `rewrite_active_update_pages()` to take the active statement and
  append-buffer statement directly instead of rediscovering them from `FILE *`.
- Leave `active_statement_and_append_buffer_for_file()` in place for generic
  raw page-buffer append paths that only have a `FILE *`.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, or file-format behavior
changes. This only narrows an internal helper boundary in the active row-DML
path.

## Single-File And Lifecycle Impact

No durable file lifecycle changes. The same active checkpoint and append buffer
own the mutation before and after this slice.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- `rewrite_active_update_pages()` no longer calls
  `active_statement_and_append_buffer_for_file()` on the prepared update path.
- Active buffered row and index-entry rewrite behavior remains covered by the
  storage and embedded storage-engine tests.
- The prepared-update benchmark completes with the expected checksum.

## Risks And Unresolved Questions

- The helper must preserve nearest active statement versus outermost append
  buffer semantics. The active statement owns statement-level undo; the
  outermost same-file active statement owns the buffered append pages.
- This does not address page-undo capture, buffered shape checks, or row-payload
  replacement costs that remain in the hot stack.
