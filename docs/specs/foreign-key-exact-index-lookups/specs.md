# Foreign-Key Exact Index Lookups

## Goal

Make MyLite foreign-key row checks probe the actual child or parent index that
belongs to the constraint. A matching key prefix in an unrelated index must not
make a child insert/update pass or a parent update/delete fail.

## Non-Goals

- New FK action support such as non-self `SET NULL`, `ON UPDATE SET NULL`,
  cascades, or `SET DEFAULT`.
- A storage catalog format change that records index numbers persistently.
- Opening and mutating a second SQL `TABLE` object from the handler.
- Exhaustive multi-table ordering matrices beyond the false-positive cases
  needed for this correctness slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/dict0mem.h:dict_foreign_t` stores both
  `foreign_index` and `referenced_index` pointers for a constraint.
- `mariadb/storage/innobase/dict/dict0dict.cc:dict_foreign_add_to_cache()`
  resolves and retains those exact index objects when loading FK metadata.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_check_foreign_constraint()`
  chooses `foreign->referenced_index` when checking child rows and
  `foreign->foreign_index` when checking referenced parent rows.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_child_foreign_key()` and
  `mylite_check_parent_foreign_key()` currently have the metadata needed to
  identify the constraint columns and referenced key name, but the non-self
  runtime path falls back to `mylite_storage_index_prefix_exists()`, which
  scans all indexes for the key prefix.

Official MariaDB documentation describes FK constraints as relationships
between indexed child columns and indexed parent columns:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Compatibility Impact

This narrows existing `RESTRICT` / `NO ACTION` enforcement to MariaDB-compatible
index selection. It should turn false successes into FK failures when the
referenced value exists only in an unrelated parent index, and turn false
parent rejections into successful updates/deletes when the value exists only in
an unrelated child index.

`docs/COMPATIBILITY.md` and the roadmap should mention exact-index FK row
checks after tests cover both directions.

## Design

Keep the on-disk FK metadata unchanged. For non-self checks, rebuild the needed
catalog `TABLE_SHARE` from the stored table definition and resolve the index at
runtime:

- child-row checks load the referenced parent share and locate the exact unique
  referenced key using the stored referenced column names and referenced key
  name;
- parent-row checks load the child share and locate the exact child supporting
  key using the stored child column names;
- self-referencing checks keep using the already-open `TABLE` object;
- all paths call the exact-index reader
  `mylite_storage_read_index_entries(..., index_number, ...)`.

The runtime share is temporary and freed before returning to MariaDB. If the
share or matching index cannot be resolved, MyLite returns the existing FK row
diagnostic rather than falling back to broad index scans.

## File Lifecycle

No primary-file format or companion-file change is introduced. The slice reads
catalog table definitions and index-entry pages already stored in the primary
`.mylite` file.

## Embedded Lifecycle And API

No public API change. The extra `TABLE_SHARE` reconstruction happens inside a
live SQL statement and uses the current THD, matching existing FK DDL
validation helpers.

## Build, Size, And Dependencies

No build-profile, binary-size, dependency, or license change.

## Test Plan

- Add a child insert/update check where the requested parent key does not
  exist, but another parent index has the same key prefix; the child write must
  fail.
- Add a parent delete check where no child FK key references the parent, but
  another child index has the same key prefix; the parent delete must succeed.
- Verify close/reopen does not lose exact-index behavior.
- Run `git diff --check`, the focused storage-engine smoke binary, and the
  storage-smoke/default CTest presets.

## Acceptance Criteria

- FK child checks use only the referenced parent index.
- FK parent checks use only the child supporting index.
- Existing self-reference behavior remains covered.
- Docs and compatibility claims match the narrowed enforcement behavior.

## Risks And Open Questions

- Runtime `TABLE_SHARE` reconstruction adds overhead to non-self FK checks.
  That is acceptable for this correctness slice; caching can be designed later
  if FK-heavy workloads show it matters.
- A future catalog-format change could store stable index identities directly,
  but the current name/column metadata is sufficient for the supported DDL
  subset and avoids a file-format migration now.
