# Foreign-Key Same-Row Action Matrix

## Goal

Cover the explicit same-row update cases for self-referencing foreign keys whose
old child key points at the same row's old parent key. MyLite already has local
pre-publication handling for the common `ON UPDATE SET NULL` and
`ON UPDATE CASCADE` cases; this slice defines and tests the matrix where the SQL
statement also changes the child FK columns.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  builds child-row update vectors for `UPDATE_SET_NULL` and `UPDATE_CASCADE`
  after finding matching child records.
- The same InnoDB path is a general recursive action executor. MyLite does not
  have that executor yet, so same-row updates are handled before index-entry and
  row-payload publication in `mariadb/storage/mylite/ha_mylite.cc`.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_apply_same_row_update_set_null()`
  and `mylite_apply_same_row_update_cascade()` already apply the action only
  when the new child key still references the old parent key.
- Explicitly changed child FK columns then continue through normal immediate
  child FK checks, including exact same-row self-reference checks for the new
  key.

## Scope

Implement coverage for this bounded SQL subset:

- durable MyLite-routed base tables;
- self-referencing `ON UPDATE SET NULL` and `ON UPDATE CASCADE`;
- old row's child FK key equals the old referenced parent key;
- simple nullable child FK columns over the current SET NULL / CASCADE table
  shapes;
- single-row updates that either leave the child FK at the old parent key,
  explicitly set it to the old parent key, set it to `NULL`, set it to the new
  same-row parent key, or set it to another existing parent key.

The supported MyLite matrix is:

| New child FK value | `ON UPDATE SET NULL` | `ON UPDATE CASCADE` |
| --- | --- | --- |
| omitted / still old parent key | action applies: child FK becomes `NULL` | action applies: child FK becomes the new parent key |
| explicit old parent key | action applies: child FK becomes `NULL` | action applies: child FK becomes the new parent key |
| explicit `NULL` | explicit value remains `NULL` | explicit value remains `NULL` |
| explicit new same-row parent key | explicit value remains the new key | explicit value remains the new key |
| explicit different existing parent key | explicit value remains that parent key | explicit value remains that parent key |

## Non-Goals

- Recursive action graphs beyond the current direct bounded action paths.
- Cyclic update-cascade parity with InnoDB.
- `SET DEFAULT`.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.
- Full InnoDB lock ordering, deadlock detection, or deferrable action graphs.

## Design

No handler algorithm change is required for the scoped matrix. The existing
same-row update phase already runs before duplicate-key checks, child FK checks,
row payload serialization, and index publication. It compares old parent and
old child key prefixes, then applies the selected action only when the new child
key still equals the old parent key.

This slice should add targeted storage-engine smoke coverage so the supported
matrix is intentional and no longer sits as an undocumented compatibility gap.
The test must also verify forced-index reads for moved child key entries and
close/reopen persistence.

## Compatibility Impact

MyLite can stop listing explicit same-row action override matrices as planned
for the existing bounded `ON UPDATE SET NULL` and `ON UPDATE CASCADE` same-row
subset. Broader recursive FK action graph behavior remains planned.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. The same-row action
matrix uses the existing row, index, and statement-checkpoint paths inside the
primary `.mylite` file.

## Public API, Build, Size, License

No public API, dependency, license, or intentional size-profile change is
introduced.

## Test And Verification Plan

- Add storage-engine smoke coverage for the same-row update matrix above.
- Verify old child-key index entries are removed and explicit new child-key
  entries are readable through forced-index scans.
- Verify rows and index entries survive close/reopen.
- Run `git diff --check`, the storage-smoke archive build, the focused
  storage-engine smoke binary, `ctest --preset storage-smoke-dev`, and
  `ctest --preset dev`.

## Acceptance Criteria

- Same-row updates that keep the child FK at the old parent key apply the
  configured FK action.
- Same-row updates that explicitly move the child FK to `NULL`, the new same
  row, or another existing parent keep that explicit value and pass ordinary FK
  checks.
- Docs and compatibility matrices distinguish this bounded same-row matrix from
  broader recursive action graphs.

## Risks And Open Questions

- This deliberately documents MyLite's bounded same-row behavior before a
  general recursive InnoDB-style action executor exists. Future recursive action
  work may need to re-evaluate this matrix against a live native InnoDB
  baseline.
