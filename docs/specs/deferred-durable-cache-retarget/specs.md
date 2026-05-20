# Deferred Durable Cache Retarget

## Problem Statement

The table-scoped durable cache retarget path keeps unrelated table caches usable
after row DML advances the durable file header. That preserves useful read
caches, but prepared update profiling shows the eager retarget work on every
row update: each mutation scans durable live-row-id, row-payload, index-leaf,
and exact-index cache sets by filename even when an active statement is open.

Durable caches are deliberately unavailable while a write statement or
transaction is active. For row DML inside an active statement, retargeting can
therefore be deferred until the statement commits. Rolled-back statements should
discard their pending durable-cache work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), as recorded in
  `docs/architecture/engineering-standards.md`.
- MyLite first-party storage code owns the relevant behavior in
  `packages/mylite-storage/src/storage.c`.
- `durable_exact_index_cache_for()`, `durable_index_leaf_page_cache_for()`,
  and `store_durable_index_leaf_page()` bypass durable caches while
  `active_statement_for_any_owner(filename)` is non-`NULL`.
- `durable_live_row_id_cache_available()` and
  `durable_row_payload_cache_available()` likewise reject active statements and
  read snapshots.
- `mylite_storage_append_row_with_index_entries()`,
  `update_row_with_index_entries()`, and `mylite_storage_delete_row()` call
  `retarget_durable_caches_after_table_mutation()` after successful row DML.
- `mylite_storage_commit_statement()` already distinguishes nested statements
  from top-level statements and promotes active caches to durable caches only
  for a successful top-level commit.
- `mylite_storage_rollback_statement()` discards nested statement state and
  clears parent active caches when a savepoint rolls back.

## Proposed Design

Track pending durable-cache retarget work on `mylite_storage_statement`:

- Row DML with no active write statement keeps the current eager retarget path.
- Row DML inside an active statement records the mutated table id and latest
  post-mutation header on that statement instead of scanning durable caches.
- Nested commit merges pending durable-cache work into the parent statement.
- Nested rollback discards pending durable-cache work because the mutation did
  not survive the savepoint.
- Top-level commit applies pending durable-cache work once, before promoting
  active exact-index and live-row-id caches to durable caches.
- Multiple table ids inside one active statement fall back to a full durable
  cache clear for that file at commit. Single-table statements keep the existing
  table-scoped retarget behavior.

## Affected Subsystems

- First-party MyLite storage runtime only.
- Statement commit and rollback lifecycle.
- Durable live-row-id, row-payload, index-leaf, and exact-index cache
  invalidation/retarget timing.

## Compatibility Impact

No SQL, C API, storage-engine routing, DDL metadata, wire-protocol, or file
format behavior changes. Durable process-local caches are an implementation
detail and remain unavailable while active statements are open.

## Single-File And Embedded Lifecycle

No durable file or companion-file behavior changes. The pending retarget state
lives on an active statement and is applied only after the statement commits.
Rollback discards it with the rest of the statement-owned runtime state.

## Binary Size, License, And Dependencies

The change adds small first-party statement bookkeeping helpers and no
dependencies. Binary-size impact should be negligible.

## Test And Verification Plan

- Add storage regression coverage for durable cache retarget across committed
  transactions and savepoint rollback.
- Build storage-smoke targets:
  `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded tests.
- Run the full `storage-smoke-dev` CTest suite.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared-update performance baseline and capture a focused sample.

## Acceptance Criteria

- Row DML outside an active statement still retargets durable caches eagerly.
- Row DML inside an active statement applies one retarget at top-level commit,
  not one scan per row mutation.
- Nested commit merges pending retarget state into the parent.
- Nested rollback discards pending retarget state.
- Multiple-table active statements conservatively clear durable caches for the
  file at top-level commit.
- Existing storage, embedded lifecycle, and storage-engine tests pass.
- Prepared-update profiling no longer shows per-row durable cache retarget scans
  as a hot path.

## Risks And Open Questions

- Applying deferred retarget after active-cache promotion would clear or retarget
  newly promoted caches. The commit path must apply deferred durable-cache work
  before promotion.
- A multi-table transaction could preserve more unrelated durable caches with a
  small table-id set. This slice chooses a conservative full-file clear for
  multiple table ids to keep the implementation bounded.
