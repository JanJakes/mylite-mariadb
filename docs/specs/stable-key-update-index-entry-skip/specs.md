# Stable-Key Update Index-Entry Skip

## Problem

Hot prepared updates that change only non-indexed columns, such as:

```sql
UPDATE app_rows SET pad = ? WHERE id = ?
```

do not need any durable index-entry maintenance, but the MyLite handler still
serializes every index key image before calling storage. Current local
profiling also shows samples in `ha_mylite::update_row()`,
`mylite_prepare_index_entries_with_scratch()`, and `key_copy()` on indexed
updates; this slice removes the same work for the stable-key subset and leaves
indexed-key maintenance as a separate performance slice.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_update.cc` executes the normal handler update path after
  evaluating assignments. The handler receives old and new record buffers plus
  `TABLE::write_set`, which identifies fields written by the statement.
- `mariadb/storage/mylite/ha_mylite.cc` already uses `TABLE::write_set` in
  `mylite_key_fields_may_change()` to avoid old-key reconstruction for some
  indexes whose key-part fields were not written.
- `ha_mylite::update_row()` still calls
  `mylite_prepare_index_entries_with_scratch()` for all keys before that
  changed-key check, so stable-key updates still invoke MariaDB `key_copy()` for
  every key.
- Handler-time write-set metadata can be broader than the serialized MyLite key
  image needs, so the final gate must compare the old/new record bytes for
  supported user-defined key parts rather than relying only on `write_set`.
- `packages/mylite-storage/src/storage.c` already treats unchanged index entries
  as row-only durable updates when `index_entry_changed[]` contains no changed
  entries; the remaining storage work that needs attention is active
  exact-index cache maintenance when no new key bytes are supplied.

## Design

Add a durable handler/storage path for updates where all durable index entries
are known to be unchanged:

1. In `ha_mylite::update_row()`, after foreign-key same-row actions have had a
   chance to alter the new record, detect whether all supported user-defined
   key-part bytes are identical between the old and new record buffers.
2. For durable rows only, skip new index-entry serialization, duplicate-key
   checks, and changed-key bitmap allocation when no indexed field can change.
3. Add a MyLite storage API that updates the row payload while preserving
   existing durable index entries.
4. In storage, keep the existing row-only append/rewrite behavior, but retarget
   active exact-index cache row ids for the table without needing key bytes.

The volatile MEMORY/HEAP path keeps the existing full index-entry rebuild. Its
in-memory row representation stores index entries on each replacement row, so a
separate volatile stable-key path would need a different row-copy helper and is
not needed for the current durable benchmark.

## Scope

In scope:

- Durable MyLite table updates where no supported key-part field is written.
- Primary, unique, and secondary indexes whose key parts are ordinary stored
  fields.
- Active exact-index cache row-id retargeting for unchanged keys.
- Prepared primary-key update coverage and indexed-read correctness after the
  stable-key update.

Out of scope:

- Updates to indexed fields.
- Generated or virtual key parts.
- Volatile MEMORY/HEAP row replacement optimization.
- Direct executor update pushdown.
- Multi-page navigable index work.

## Compatibility Impact

No SQL semantics change. The fast path applies only after MariaDB has already
evaluated assignments and populated the normal record buffers. If an indexed
field, generated key part, or uncertain write set is involved, the handler uses
the existing full index-entry path with duplicate-key checks.

Skipping duplicate-key checks is safe only for the stable-key case because no
index key image can change. Foreign-key checks and actions continue to run
through the existing handler code.

## Single-File And Embedded Impact

No file-format or companion-file lifecycle change. Durable index pages remain
unchanged for stable-key updates; row pages and row-state pages continue to use
the current statement checkpoint, journal, rollback, and active append-buffer
rewrite paths.

## Public API, License, And Binary Size

The storage package gains one first-party C API for preserving existing index
entries during row update. It does not expose MariaDB internals, add
dependencies, or alter licensing. Binary-size impact should be negligible.

## Test Plan

- Add routed-storage embedded coverage for repeated prepared primary-key
  updates that change only a non-indexed column.
- Add an indexed read after a stable-key update so active exact-index cache
  retargeting is covered.
- Build `mysqlserver`, `mylite_storage_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff` on touched files.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`
  before committing and record the local result. The current benchmark updates
  an indexed `value` column, so it is expected to remain a follow-up indexed-key
  maintenance signal rather than direct proof for this stable-key slice.

## Acceptance Criteria

- Stable-key durable updates avoid handler key serialization and duplicate-key
  checks.
- Indexed reads after repeated stable-key updates return the updated row
  payload.
- Full indexed-field updates still use the existing duplicate-checking path.
- Relevant builds, tests, formatting, and diff checks pass.
- Existing prepared-update performance stays neutral, and profiling clearly
  leaves indexed-key update maintenance as the next bottleneck.

## Risks

- `TABLE::write_set` must be treated conservatively. Missing write-set data,
  generated key parts, unsupported indexes, or any written key-part field must
  fall back to the existing full path.
- Active exact-index cache retargeting must update row-id buckets in place; a
  stale cache entry would affect repeated point updates inside a transaction.
