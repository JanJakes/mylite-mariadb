# Ownerless File-Operation Path Normalization

## Problem

Ownerless native recovery can arm InnoDB uncheckpointed file-operation recovery
after ownerless DDL when retained WAL, a native file-operation checkpoint
marker, or the saved redo-header prefix proves that prior ownerless
redo/checkpoint suppression may have left native FILE redo below a durable
checkpoint boundary.

Repeated `test_ownerless_schema_lifecycle_refreshes_peer_dictionary` coverage
surfaced an intermittent recovery failure where InnoDB attempted to open a
tablespace path shaped like:

```text
<datadir>//tmp/.../<database>.mylite/datadir/ownerless_schema/ownerless_schema_table.ibd
```

That shape means a redo FILE record carried a datadir-prefixed path with the
leading path separator stripped, and recovery treated it as a normal relative
schema/table path before prepending the active datadir.

## Source Findings

Base source authority: MariaDB 11.8.6 import
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/storage/innobase/fil/fil0fil.cc`:
  `mtr_t::log_file_op()` writes FILE_CREATE, FILE_DELETE, FILE_RENAME, and
  FILE_MODIFY redo names. MyLite already intercepts this path to log datadir
  paths as schema/table-relative names while ownerless relative FILE redo mode
  is enabled.
- `mariadb/storage/innobase/log/log0recv.cc`: `fil_name_process()` resolves
  FILE redo names during recovery. MyLite already resolves non-absolute names
  relative to the active datadir while uncheckpointed file-operation recovery
  is armed.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` owns
  the ownerless InnoDB hook flags used by both call sites.

The write and recovery sides need the same datadir-prefix normalization rule;
otherwise a non-canonical datadir-prefixed name can escape the write-side
relative conversion and later be prefixed again during recovery.

## Design

Add one MyLite-owned InnoDB hook helper:

```c
int mylite_ownerless_innodb_file_op_redo_relative_path(
    const char *datadir,
    const char *path,
    char *relative_path,
    size_t relative_path_size);
```

The helper returns a schema/table-relative `.ibd` path only when:

- `path` is under `datadir`,
- or `path` is under the same datadir with leading path separators stripped,
- the resulting path contains at least one path separator,
- the resulting path ends in `.ibd`,
- and the caller-provided output buffer can hold the result.

`fil0fil.cc` uses the helper before writing FILE redo while relative FILE redo
mode is enabled. `log0recv.cc` uses the same helper before prepending datadir
to a non-absolute FILE redo name while ownerless uncheckpointed file-operation
recovery is armed. Existing ordinary relative schema/table names still use the
current datadir-prefix recovery rule.

## Compatibility Impact

This changes only MyLite ownerless native DDL recovery internals. SQL behavior,
public C API, and directory layout do not change.

The compatibility improvement is that ownerless DDL recovery tolerates both the
intended relative FILE redo names and the observed leading-separator-stripped
datadir-prefixed form without constructing a path outside the valid
single-directory datadir subtree.

## Database Directory And Native Storage Impact

Durable state remains inside the MyLite database directory. The fix prevents an
invalid recovery path from being constructed as a child of `datadir/` when the
redo record already encodes that datadir path without its leading separator.

No new files, locks, segments, or external durable state are introduced.

## Tests

- Extend `mylite_embedded_ownerless_innodb_lock_hooks_test` with deterministic
  coverage for exact datadir prefixes, trailing/double separators,
  leading-separator-stripped datadir prefixes, outside-datadir paths, non-`.ibd`
  paths, basename-only `.ibd` paths, and undersized output buffers.
- Rerun `mylite_ownerless_cross_process_sql_test sql-case 102` repeatedly as
  live DDL schema lifecycle integration coverage.
- Rerun the ownerless DDL/file-operation and focused ownerless SQL selectors
  that cover native file-operation recovery and recent insert fast-path changes.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- FILE redo path normalization is shared between redo logging and recovery.
- The observed leading-separator-stripped datadir prefix normalizes to the same
  schema/table-relative `.ibd` path as the canonical absolute datadir path.
- Ownerless schema lifecycle DDL recovery no longer constructs
  `<datadir>/<datadir-without-leading-slash>/...` paths.
- Focused hook, ownerless SQL, and formatting checks pass.

## Risks

The live SQL failure is intermittent, so deterministic helper coverage carries
the main proof for the exact malformed path class. Broader randomized DDL
oracle stress and exhaustive DDL/file lifecycle recovery remain planned.
