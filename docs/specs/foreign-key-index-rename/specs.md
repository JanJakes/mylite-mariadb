# Foreign-Key Index Rename

## Goal

Support `ALTER TABLE ... RENAME INDEX` for a parent unique secondary key that is
referenced by MyLite's supported public foreign-key subset. The renamed parent
key must remain the FK's referenced key in MyLite catalog metadata, row checks,
information schema, and close/reopen discovery.

## Non-Goals

- Do not add primary-key rename support.
- Do not implement online, in-place, instant, or lock-free index rename.
- Do not support cascading actions, `SET NULL`, `SET DEFAULT`, deferrable
  checks, partitioned FKs, or volatile zero-file FK tables.
- Do not implement broad multi-rename conflict matrices beyond MariaDB's
  existing validation.
- Do not make child supporting-key renames store child-key names; MyLite's
  current FK metadata stores child columns, not child key names.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_class.h:Alter_rename_key` stores `old_name`, `new_name`,
  and `alter_if_exists` for parsed `RENAME INDEX` / `RENAME KEY` clauses.
- `mariadb/sql/sql_alter.h:Alter_info::alter_rename_key_list` carries those
  rename requests through ALTER preparation.
- `mariadb/sql/sql_table.cc:9151-9175` rewrites retained key names in the
  rebuilt table definition during copy ALTER and rejects primary-key rename.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_validate_retained_parent_foreign_key()`
  currently requires the stored `referenced_key_name` to remain present in the
  rebuilt parent `TABLE`.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_store_foreign_key_definitions()`
  records the validated parent key name in MyLite FK metadata for row checks
  and handler FK metadata hooks.
- `packages/mylite-storage/src/storage.c` can store, read, list, drop, and
  rename FK table identities, but before this slice it did not update a FK
  metadata blob's referenced key name.

## Compatibility Impact

`ALTER TABLE ... RENAME INDEX` remains partial, but the supported copy-rebuild
subset now includes parent unique secondary keys referenced by MyLite-supported
FKs. The FK continues to reference the same parent columns and table; only the
parent key name changes to match MariaDB's rebuilt definition.

Child supporting-key renames should already work because MyLite FK metadata
stores child column names and validates the rebuilt child table by column
prefix. This slice targets the stored parent key name because parent row checks
and `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS.UNIQUE_CONSTRAINT_NAME` depend
on it.

## Design

1. During retained parent FK validation, map a stored `referenced_key_name`
   through `Alter_info::alter_rename_key_list` before checking the rebuilt
   parent table. This lets validation accept a supported parent key whose
   columns are unchanged but whose name is changing in the same ALTER.
2. While MariaDB prepares parent FK metadata for the active ALTER, expose the
   renamed referenced key name so SQL-layer copy-ALTER validation sees the
   rebuilt parent key name. Restrict the fallback through `current_thd` to
   active ALTER/CREATE INDEX/DROP INDEX commands so ordinary DML prelocking
   never reads a stale LEX rename list.
3. After the rebuilt table definition is stored, update parent-listable FK
   metadata records whose referenced key name was renamed by the same ALTER.
4. Add a storage API that rewrites one FK metadata blob with a new
   `referenced_key_name` in a single catalog publication. It should append a new
   FK metadata blob, rewrite the FK catalog record to point at the new blob,
   preserve child/parent table identities and column/action/nullability
   metadata, and leave old blob pages orphaned until compaction.
5. Keep statement-checkpoint rollback behavior: if the ALTER fails after the
   metadata rewrite, the statement rollback restores the original FK metadata
   and table definition.

## Affected Subsystems

- `packages/mylite-storage/`: FK metadata blob rewrite primitive and storage
  unit coverage.
- `mariadb/storage/mylite/`: retained parent FK validation and metadata update
  during copy ALTER.
- `packages/libmylite/tests/`: SQL-level storage-smoke coverage for parent
  unique-key rename under an existing FK.
- Docs and compatibility matrices describing `ALTER TABLE ... RENAME INDEX`.

## Single-File And Embedded Lifecycle

The update stays inside the primary `.mylite` file. No persistent `.frm`,
`.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or
plugin-owned durable sidecar is introduced. Old FK metadata blob pages become
unreachable internal free-space debt.

No public `libmylite` C API change is needed. The new storage function is an
internal first-party primitive used by the MariaDB handler.

## Build, Size, And Dependencies

No dependency or size-profile change is intended. The default size-reduction
roadmap work remains out of scope for this pass.

## Test Plan

- Storage unit coverage for updating one FK record's referenced key name while
  preserving column lists, actions, table identities, and parent-list lookup.
- SQL storage-smoke coverage:
  - create a parent table with a referenced unique secondary key;
  - create a child table with a supported FK referencing that key;
  - rename the parent key with `ALGORITHM=COPY`;
  - verify information-schema exposes the new unique constraint name;
  - verify child missing-parent checks and parent update/delete checks still
    use the renamed key;
  - verify old key hints fail and new key hints work before and after reopen.
- Run the storage build, focused storage test, focused embedded storage-engine
  test, storage-smoke CTest preset, default CTest preset, and `git diff --check`.

## Acceptance Criteria

- Parent unique secondary key rename succeeds for a referenced supported FK.
- Retained parent FK validation rejects only truly missing/invalid parent keys,
  not a same-ALTER supported key rename.
- FK metadata stores the renamed referenced key name.
- Row checks and handler metadata remain correct after close/reopen.
- Unsupported primary-key rename, broader FK actions, and online/in-place ALTER
  remain explicitly unsupported or planned.

## Implementation Status

Implemented in this slice:

- MyLite maps retained parent FK referenced key names through active ALTER key
  rename lists during validation and parent FK metadata listing.
- MyLite updates the stored FK referenced key name after the parent table
  rebuild is published.
- Storage unit coverage rewrites an FK metadata blob's referenced key name while
  preserving table identities, columns, actions, nullable metadata, and parent
  FK listing.
- Embedded storage-engine coverage verifies SQL-level parent unique secondary
  key rename under a supported FK before and after close/reopen.

## Risks And Open Questions

- The metadata rewrite appends a new FK blob and leaves the old one orphaned,
  matching current dropped/rewritten metadata behavior until compaction.
- Multi-rename and mixed ADD/DROP/RENAME FK matrices remain future coverage.
- Broader `foreign_key_checks=0` import semantics are unchanged.
