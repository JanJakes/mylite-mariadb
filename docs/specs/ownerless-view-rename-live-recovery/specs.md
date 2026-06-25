# Ownerless View Rename Live Recovery

## Problem Statement

Ownerless live recovery covers table `RENAME TABLE` forms when InnoDB native
file-operation redo proves the durable file move. It also covers many
metadata-only view DDL forms, but view-only `RENAME TABLE` still sat in the
planned bucket because it uses the `RENAME TABLE` syntax while not producing an
InnoDB file-operation marker.

MyLite should recover a writer killed after a completed view rename but before
ownerless dictionary finish while another ownerless peer remains live. It must
not broaden marker-free table rename recovery for native table files.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB routes view rename through `RENAME TABLE` syntax, so token shape alone
  cannot distinguish table and view renames.
- Existing MyLite table rename recovery uses
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE` and requires durable
  native file-operation marker evidence.
- Existing view create/drop/replace/alter recovery kinds are metadata-only and
  recover live without native file-operation marker evidence.
- `database.cc` already uses internal `information_schema` probes for bounded
  ownerless metadata policy decisions.

## Design

- Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_VIEW` as a metadata-only
  dictionary recovery kind.
- Keep the existing syntactic `RENAME TABLE` classifier for table rename.
- Before executing a rename statement, inspect every source object from the
  already accepted rename-list shape in `information_schema.views`.
- Use the metadata-only view-rename kind only when every source object is
  proven to be a view. Missing metadata, query failure, missing current schema,
  or any table source falls back to the existing table-rename kind.
- Include the new kind in dictionary-state validation, active-owner
  metadata-only marking, and dead-owner metadata-only recovery.

## Scope

In scope:

- View-only `RENAME TABLE source_view TO target_view` live recovery.
- Marker-clear recovery while another ownerless peer remains live.
- Source view absence, target view presence, base table preservation, view reads,
  base-table writes reflected through the renamed view, ownerless/native reopen,
  and forced `.shm` rebuild.

Out of scope:

- Mixed table/view rename lists.
- Temporary-table rename.
- `RENAME TABLE IF EXISTS`.
- Crash injection inside MariaDB's rename loop or DDL-log rollback.
- Broader table rename semantics beyond the existing file-op-marker path.

## Compatibility Impact

Successful SQL behavior is unchanged. A killed ownerless writer at
`dictionary-before-finish` after a view-only rename can now be recovered by a
live ownerless opener without requiring a native file-operation marker. Native
table rename recovery remains marker-gated.

## Verification Plan

- Build hook targets for ownerless primitives and ownerless SQL.
- Run focused hook selector `dictionary-view-rename-crash`.
- Run adjacent view metadata selectors and rename marker selectors.
- Run ownerless primitives.
- Run production ownerless SQL selectors for view DDL and rename/table DDL.
- Run ownerless DDL stress because recovery-kind selection changed.
- Run format, CI production-build, and diff checks.

## Acceptance Criteria

- View rename records a metadata-only recoverable dictionary marker and recovers
  while a peer remains live.
- The native file-operation marker stays clear before live recovery, after live
  recovery, and after final no-live reopen.
- The source view is absent and the target view remains queryable through
  ownerless/native reopen before and after forced `.shm` rebuild.
- Table rename marker-required behavior and adjacent view metadata recovery stay
  green.
