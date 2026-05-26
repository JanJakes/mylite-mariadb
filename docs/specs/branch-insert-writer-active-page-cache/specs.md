# Branch Insert Writer Active Page Cache

## Problem

After active cache refresh stopped recomputing checksums, prepared insert
sampling still shows hot checksum validation inside single-level maintained
branch inserts:

- `insert_branch_index_leaf_entry()` decodes the branch root page with
  `decode_index_branch_page()` after the planning phase has already decoded and
  cached that branch page.
- The same writer decodes the selected leaf page with `decode_index_leaf_page()`
  after planning has already read and cached that leaf page.
- `refresh_index_branch_child_after_leaf_insert()` remains a separate validation
  cost after the writer mutates branch bytes.

The first two decodes are avoidable for the common prepared-insert path where
planning and writing happen in the same active statement.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `plan_maintained_index_root_inserts()` and its single-level branch planning
  path already use `read_cached_active_index_branch_page()` and
  `read_cached_active_index_leaf_page()`, storing decoded pages in the active
  statement cache on misses.
- `insert_branch_index_leaf_entry()` bypasses those active caches and performs
  fresh pager reads plus full branch and leaf decodes before mutating the leaf.
- Active cache entries are refreshed after maintained insert writes, so repeated
  same-statement inserts should see the latest branch and leaf bytes.

## Design

Add writer-local read helpers for single-level branch inserts:

- Read the branch root from the active branch page cache first.
- Read the selected leaf from the active leaf page cache first.
- On cache miss, preserve the current behavior: read through the pager, decode
  with the durable checksum-validating decoder, and seed the active cache.

Use those helpers in `insert_branch_index_leaf_entry()`. Do not change deeper
branch insert writers or branch-child refresh validation in this slice.

## Compatibility Impact

No SQL-visible or public API behavior changes. The fallback path still uses the
durable decoders, and cached pages are scoped to the active statement that is
already planning and writing the insert.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. The slice only reuses transient
statement-local cache entries that are already invalidated by statement cleanup,
rollback, and maintained-index page writes.

## Binary-Size And Dependency Impact

No dependency changes. The code adds two small first-party helper paths and
test counters.

## Test And Verification Plan

- Extend the active branch leaf cache test to assert that same-statement
  single-level branch inserts do not need writer fallback decodes when planning
  has populated the active page caches.
- Keep storage and storage-smoke tests passing.
- Run `git diff --check`, `git clang-format --diff`, and a prepared insert
  component benchmark.

## Acceptance Criteria

- `insert_branch_index_leaf_entry()` uses active branch and leaf page caches
  before durable pager reads and decodes.
- Cache misses still run the same durable decode and seed the active cache.
- Existing branch insert behavior and rollback safety remain covered by tests.
- Relevant tests, formatting checks, and the prepared insert component
  benchmark pass.

## Risks

- This does not remove the post-mutation branch refresh decoder. If that remains
  hot, the next slice should replace that validation with a targeted
  single-cell branch metadata update check.
