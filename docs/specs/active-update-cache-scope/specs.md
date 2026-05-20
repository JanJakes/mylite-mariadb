# Active Update Cache Scope

## Problem

Prepared primary-key updates in one transaction now spend a visible share of
storage-side time rediscovering the same active statement/cache owners. The hot
path repeatedly walks the active statement chain and compares filenames while
validating the target row and maintaining active exact-index, live-row-id, and
row-payload caches after each update.

The current 1M-iteration prepared-update sample shows the storage side mostly
below MariaDB execution overhead, but the remaining MyLite-owned cost includes
`active_statement_for_file()`, `active_*_cache_statement_for()`, and related
`strcmp()` calls inside update validation and cache maintenance.

## Source Findings

- MariaDB base line: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned source: `packages/mylite-storage/src/storage.c`.
- `active_statement_for_file()` returns the innermost active statement for the
  current owner and file handle.
- Active exact-index, live-row-id, row-payload, and table-entry cache owner
  lookups all use the same outermost matching active statement for the current
  owner and filename.
- `update_row_with_index_entries()` already has both the durable file handle
  and filename for the full update operation, so it can resolve those active
  statement owners once and pass them through local helpers.

## Design

Add statement-scoped variants for the hot update helpers:

- validate direct live rows against a supplied active file statement;
- seed and maintain active live-row-id caches against a supplied active cache
  statement;
- maintain active exact-index and row-payload caches against the same supplied
  active cache statement;
- keep the existing filename/file wrappers for callers that do not already have
  a resolved statement.

Consolidate the duplicated active cache-owner lookup into one private helper.
The helper preserves the existing behavior of selecting the outermost matching
statement for the current owner.

## Compatibility Impact

No SQL-visible behavior change.

## File And API Impact

No public API, storage format, or companion-file change.

## Storage Routing Impact

No routing change.

## Binary-Size Impact

Small private-code increase from statement-scoped helper wrappers.

## Test And Verification Plan

- Build first-party storage smoke targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run prepared-update benchmark repeats to confirm the hot path remains stable.

## Acceptance Criteria

- Existing storage-smoke coverage remains green.
- Statement/cache ownership is unchanged for generic callers.
- Prepared-update performance does not regress materially.

## Risks

- The live-row cache and cache-owner helpers intentionally use different active
  statement scopes. The update path must pass the file-scoped innermost
  statement for live-row validation and the filename-scoped outermost statement
  for cache promotion.
