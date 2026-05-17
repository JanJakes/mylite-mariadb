# Foreign-Key Same-Row Self-Reference

## Goal

Support the next narrow self-referential foreign-key compatibility case:
a single inserted or updated row may satisfy its own immediate `RESTRICT` /
`NO ACTION` constraint when the child key prefix exactly matches the same
row's referenced unique parent key prefix.

This extends the existing self-reference subset without claiming broader
statement ordering. Multi-row statements where one row depends on another row
that has not been published yet, cascades, `SET NULL`, and `SET DEFAULT` remain
separate work.

## Non-Goals

- Ordering arbitrary multi-row self-referential inserts or updates.
- Deferrable constraints.
- Recursive cascades, `SET NULL`, or `SET DEFAULT`.
- Changing self-referential parent delete/update semantics beyond the current
  immediate `RESTRICT` / `NO ACTION` checks.
- Supporting partitioned, volatile, temporary, or row-discarding FK tables.
- Reworking the storage publication order for all row/index entries.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_clust_index_entry()`
  checks foreign constraints before inserting a clustered index entry.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_sec_index_entry()`
  checks foreign constraints before inserting a secondary index entry.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_check_foreign_constraints()`
  checks only constraints whose child index matches the index being inserted,
  or primary-key-derived child references when no separate child index exists.
  For the common self-reference shape with a primary parent key and secondary
  child key, this lets the clustered parent entry exist before the secondary
  child FK check runs.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_check_foreign_constraint()`
  skips checks when FK columns are `NULL` and returns `DB_NO_REFERENCED_ROW`
  when a referenced row is not visible.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_child_foreign_key()`
  checks parent existence before MyLite publishes the new row and index
  entries. That pre-publication ordering currently rejects same-row
  self-references even when the new row's child and referenced key prefixes
  match.

## Compatibility Impact

The supported FK subset gains same-row self-referencing inserts and updates for
the validated durable MyLite-routed table shapes. Compatibility remains
partial:

- covered: a row whose child key prefix equals its own referenced unique parent
  key prefix;
- still planned: ordered multi-row FK checks, recursive action execution,
  cascades, `SET NULL`, `SET DEFAULT`, deferrable checks, and broader
  transaction-aware FK validation.

## Design

Keep MyLite's current handler publication order. In the child FK check path:

1. Build the child key prefix exactly as today.
2. When the FK metadata references the same table, resolve the referenced
   parent key from the same `TABLE`.
3. Build the referenced parent key prefix from the same new row buffer, using
   the stored referenced nullable-key bitmap.
4. If the child and parent prefixes are byte-identical, treat the parent row as
   satisfied by the row being inserted or updated and skip the storage lookup.
5. If the prefixes do not match, use the existing storage-backed parent lookup.

This keeps the change local to child existence checks. Parent update/delete
checks remain immediate `RESTRICT` / `NO ACTION` checks over live child index
prefixes, so broader self-action semantics stay explicit future work.

## Single-File And Embedded Lifecycle

No file-format or lifecycle change is required. Rows, index entries, and FK
metadata remain durable in the primary `.mylite` file. The slice adds
close/reopen coverage to prove same-row self-reference metadata and row checks
continue to work after catalog discovery.

## Storage-Engine Routing Impact

The behavior applies only to durable MyLite-routed base tables in the supported
FK subset, including omitted-engine and routed `ENGINE=InnoDB` tables. Volatile
and row-discarding FK tables remain rejected at FK DDL validation time.

## Public API, Wire-Protocol, Build, And Dependency Impact

No public C API, wire-protocol, dependency, or binary-size change is expected.
Applications see the behavior through ordinary direct and prepared SQL
execution diagnostics.

## Test And Verification Plan

- Add storage-smoke direct SQL coverage in
  `packages/libmylite/tests/embedded_storage_engine_test.c`.
- Cover same-row insert before and after close/reopen.
- Keep missing-parent failure coverage to prove only exact same-row matches are
  allowed.
- Keep existing self-reference truncate and referenced-parent rejection
  coverage.
- Run the MariaDB MyLite storage archive build because handler code changes.
- Run embedded storage/exec/statement smoke tests.
- Run default first-party format and storage checks.
- Run `git diff --check`.

## Acceptance Criteria

- Same-row self-referencing inserts succeed when the child key prefix exactly
  matches the same row's referenced unique parent key prefix.
- Missing-parent self-referencing inserts still fail.
- Existing-parent self-references, parent update/delete checks, truncate, and
  close/reopen behavior keep passing.
- Docs and compatibility matrices distinguish same-row self-reference support
  from multi-row ordering and action semantics.

## Risks And Open Questions

- Self-referential parent delete/update behavior may need finer compatibility
  work later if MySQL/MariaDB allow final-state-preserving self mutations that
  MyLite's current immediate parent checks reject.
- Multi-row ordering is not addressed by byte-prefix equality against the same
  row buffer; it needs statement-level planning or deferred validation.
