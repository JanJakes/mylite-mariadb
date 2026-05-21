# Static Leaf Prefix Lower Bound

## Problem

`mylite_storage_index_prefix_exists_for_index()` can now answer durable FK
prefix probes from complete published leaf roots, but the static leaf scan walks
each candidate page from entry zero. Exact leaf lookups already lower-bound
within a sorted leaf page before collecting matches; prefix probes should avoid
the same avoidable per-page linear prefix walk.

This matters most for composite-key FK checks, where the checked prefix can be
shorter than the full serialized index key.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_index_prefix_exists()` routes
  durable FK parent/child probes to
  `mylite_storage_index_prefix_exists_for_index()`.
- `packages/mylite-storage/src/storage.c::find_index_leaf_page_first_match_row_id()`,
  `append_index_leaf_matches_to_row_id_list()`, and
  `append_index_leaf_matches_to_entryset()` already binary-search exact keys
  within one sorted leaf page.
- `packages/mylite-storage/src/storage.c::scan_static_index_leaf_prefix_exists()`
  currently scans every entry in each candidate page until it reaches the
  prefix.

## Scope

- Add a page-local lower-bound helper for serialized key prefixes.
- Use it in the static leaf prefix-exists scan.
- Cover shorter-than-full-key prefixes in storage tests.
- Keep append-tail and missing-root fallback behavior unchanged.

## Non-Goals

- No file-format change.
- No new public API.
- No B-tree split, merge, or multi-page maintained-index mutation.
- No collation-sensitive comparison change; the helper keeps the existing raw
  serialized key-prefix comparison.

## Design

Add `find_index_leaf_page_prefix_lower_bound()` beside the existing leaf-page
exact-match helpers. It compares only the requested prefix byte count and
returns the first entry whose key prefix is greater than or equal to the probe
prefix.

`scan_static_index_leaf_prefix_exists()` will:

- reject pages whose fixed key width is shorter than the requested prefix;
- start at the lower-bound entry for each page;
- stop immediately when the first non-matching greater prefix is observed;
- honor the existing skipped row id rule.

The helper works for both immutable leaf pages and maintained-root pages because
maintained roots are exposed through the same decoded leaf-page view.

## Compatibility Impact

SQL-visible FK behavior should not change. The helper only changes how MyLite
finds the first candidate entry inside a sorted static page.

## Single-File And Lifecycle Impact

No lifecycle change. The slice reads existing `.mylite` pages and introduces no
companion files.

## Public API And File-Format Impact

No public `libmylite` API, first-party storage API, or file-format change.

## Storage-Routing Impact

Durable MyLite FK prefix probes over complete static roots get a cheaper
page-local search. Volatile MEMORY/HEAP routed tables and append-tail fallback
roots keep their existing paths.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to a small
first-party helper.

## Test Plan

- Extend static leaf prefix tests with shorter-than-full-key prefixes:
  - a prefix matching multiple rows;
  - skipped row id still leaves another matching row visible;
  - missing prefix returns false.
- Re-run storage unit coverage and the storage-engine smoke.
- Run `git diff --check` and `git clang-format --diff` for touched C/C++ files.

## Acceptance Criteria

- Static prefix-exists scans lower-bound within each leaf page before checking
  candidate entries.
- Exact, full-index, and append-tail fallback behavior stays unchanged.
- Tests cover full-key and shorter-prefix probes.

## Verification Results

2026-05-21, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

All passed.

## Risks And Open Questions

- This is a local page-search improvement, not the planned multi-page
  maintained B-tree implementation.
- Prefix comparisons remain byte-oriented because they operate on handler-built
  serialized key images.
