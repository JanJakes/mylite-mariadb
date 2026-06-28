# Ownerless Rename IF EXISTS Target-Conflict Recovery

## Problem Statement

Ownerless `RENAME TABLE IF EXISTS` coverage proves missing-source warning/no-op
lists and native rename rollback for several same-schema and cross-schema
shapes. The target-conflict branch still needed classification. Local evidence
showed that the SQL reaches the ownerless `dictionary-before-finish` lifecycle
hook after MariaDB returns target-exists errno `1050`, but does not reach the
native rename file-operation hook. A killed process at that boundary must not
leave the dictionary generation unrecoverable.

## Source Findings

- Base source authority: MariaDB 11.8 import `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. Missing
  source tables return `-1` under `IF EXISTS`, allowing the caller to skip that
  pair. Existing targets call `my_error(ER_TABLE_EXISTS_ERROR, ...)` and
  return `1`; the source comment says this branch cannot be skipped.
- `mariadb/mysql-test/main/rename.test:196-207` and
  `mariadb/mysql-test/main/rename.result:203-216` confirm the SQL oracle:
  multiple missing `IF EXISTS` sources emit note `1146`, but a later
  target-exists conflict still fails with `ER_TABLE_EXISTS_ERROR`.
- MyLite's ownerless dictionary lifecycle begins before executing classified
  DDL and finishes after MariaDB returns. Therefore a failed pre-native DDL can
  still be killed at `dictionary-before-finish`.

## Design

Add a metadata-only ownerless recovery kind for failed dictionary DDL that did
not produce InnoDB native file-operation redo. Before finishing a failed
dictionary DDL statement, MyLite now checks the native file-op redo flag:

- if native file-op redo is present, it restores that flag and keeps the
  existing file-operation marker/recovery path;
- if the recovery kind already forces a native marker, it keeps the existing
  recovery kind;
- otherwise it marks the active dictionary generation as failed-DDL no-op
  recovery so a dead owner can be cleaned up by a live peer.

The target-conflict selector uses this deterministic same-schema list:

```sql
RENAME TABLE IF EXISTS
  app.missing_before TO app.missing_before_dst,
  app.source TO app.target,
  app.missing_after TO app.missing_after_dst;
```

`source` and `target` both exist, so the middle pair fails with MariaDB errno
`1050`. Hook coverage kills the writer at `dictionary-before-finish` and
verifies recovery preserves the original tables. A second selector arms
`rename-table-after-native-file-op` and proves the native hook remains
unreachable for this branch.

## Affected Subsystems

- Ownerless dictionary state recovery metadata.
- Direct and prepared failed-DDL finish paths.
- Hook-build ownerless SQL coverage.

No public API, directory-layout, wire-protocol, dependency, or binary-size
impact is expected.

## Compatibility Impact

SQL behavior remains MariaDB-compatible: `IF EXISTS` only downgrades missing
source tables to notes, while an existing target remains a hard error. The
ownerless compatibility matrix should treat target-conflict rename as a
recoverable failed-DDL dictionary boundary plus a native file-op negative proof.

## Database-Directory And Native Storage Impact

The crash selector verifies both original InnoDB tables and their `.frm`/`.ibd`
files remain present, skipped target files remain absent, the native
file-operation checkpoint-needed marker remains clear, ordinary post-error
writes succeed, and ownerless/native reopen before and after forced `.shm`
rebuild observe the same state.

## Test And Verification Plan

- Add hook selector `dictionary-rename-if-exists-target-conflict-crash`.
- Add hook selector `rename-if-exists-target-conflict-native-negative-proof`.
- Register separate CTests for crash recovery and native negative proof.
- Run both selectors directly under `ownerless-test-hooks`.
- Run the focused hook CTest regex including the table-wait negative proof.
- Run production-build guard, formatting, and whitespace checks.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after the target-conflict error
  is recoverable while another ownerless peer remains open.
- The native file-operation marker remains clear for the target-conflict
  branch.
- The `rename-table-after-native-file-op` hook is not reached.
- Original source and target metadata/files/rows are preserved, skipped targets
  are absent, post-error writes succeed, and ownerless/native reopen plus
  forced `.shm` rebuild preserve the state.
- Compatibility docs no longer list target-conflict `IF EXISTS` rename as an
  unclassified completion gap.

## Risks And Follow-Up

- This slice does not broaden mixed temporary/permanent or foreign-key rename
  recovery.
- Longer cross-schema missing-source permutations and broader DDL/file-lifecycle
  matrices remain completion work.
