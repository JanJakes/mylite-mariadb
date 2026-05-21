# Active Cache Lookup Short Circuits

## Problem

Prepared point updates still spend measurable time in small first-party lookup
helpers after the larger row/index storage paths have been optimized. Sampling
shows `active_statement_for()`, `find_active_table_entry_cache_in_statement()`,
and active row-payload cache lookup helpers on the remaining storage stack, with
`strcmp()` also visible in the top collapsed samples.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- MyLite storage keeps active statement, table-entry, exact-index, and
  row-payload caches in thread-local statement state.
- Hot prepared-update paths repeatedly resolve the same active statement chain,
  schema/table cache entry, and row-payload cache for one file and table.
- These helpers are behaviorally simple but currently pay function-call and
  string-compare overhead even when the same string objects are reused.

## Design

- Mark sampled active statement/cache helpers as hot inline storage helpers.
- Compare active statement owners before comparing filenames in owner-scoped
  lookup loops.
- Treat identical string pointers as a cache-key match before falling back to
  `strcmp()` for filenames, schema names, and table names.
- Keep all existing fallback comparisons, ownership checks, cache invalidation,
  and active statement chain traversal semantics unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Cache hits still require the same filename/schema/table values
and active owner identity.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only reduces transient lookup overhead inside existing active
statement and cache resolution.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Use benchmark samples for call-shape only when unrelated machine load is high.

## Acceptance Criteria

- Active owner-scoped lookups avoid filename `strcmp()` when owner identity does
  not match.
- Active cache lookups avoid `strcmp()` when string pointers are identical.
- Existing storage and embedded routed-storage tests pass.

## Risks

- Over-inlining can increase text size. The slice is intentionally limited to
  small sampled helpers that already sit on hot row-DML lookup paths.
