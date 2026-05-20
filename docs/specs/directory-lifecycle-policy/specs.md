# Directory Lifecycle Policy

## Goal

Define and enforce the first MyLite database-directory lifecycle policy:
creation, metadata validation, existing-directory adoption, stale runtime
cleanup, and clean-close layout.

## Non-Goals

- Do not implement cross-process locking or writer coordination.
- Do not implement metadata migrations between MyLite directory formats.
- Do not support opening arbitrary MariaDB datadirs as MyLite directories.
- Do not claim crash recovery beyond replacing stale `run/` state on a later
  clean open.
- Do not change the public `libmylite` API.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/libmylite/src/database.cc:437-470` owns durable database-directory
  creation and `MYLITE_OPEN_CREATE` behavior.
- `packages/libmylite/src/database.cc:473-503` handles existing directories,
  empty-directory initialization, missing metadata, and stale `run/` cleanup.
- `packages/libmylite/src/database.cc:506-572` validates `mylite.meta`,
  `datadir/`, and `tmp/`.
- `packages/libmylite/src/database.cc:653-680` removes `run/` and clears `tmp/`
  on final close.
- `packages/libmylite/src/database.cc:744-757` creates process-local `run/`
  and `run/plugins/` for durable paths.

MariaDB remains responsible for schema directories and native storage files
under `datadir/`; MyLite owns the outer database directory contract.

## Compatibility Impact

This slice tightens open semantics:

- A missing directory still requires `MYLITE_OPEN_CREATE`.
- A pre-existing empty directory can be initialized only with
  `MYLITE_OPEN_CREATE`.
- A pre-existing empty directory without `MYLITE_OPEN_CREATE` returns
  `MYLITE_NOTFOUND`.
- A non-empty directory without `mylite.meta`, invalid metadata, or missing
  required layout directories returns `MYLITE_CORRUPT`.
- A valid directory may have stale `run/` state from an unclean shutdown; MyLite
  replaces it on open when no runtime is already active.

## Design

`mylite.meta` is the directory identity marker for format 1:

```text
format=1
mariadb_base=mariadb-11.8.6
```

The durable format-1 layout requires:

```text
<name>.mylite/
  mylite.meta
  datadir/
  tmp/
```

The later locking slice adds `mylite.lock` as a stable advisory lock anchor.

`run/` is runtime state. It exists while the embedded runtime is active and is
removed on final close. On open, stale `run/` is removed before runtime startup
only when no MyLite runtime is already active in the process; opening a second
handle to the same active directory must preserve the live `run/`.

## File Lifecycle

- New missing paths are created only with `MYLITE_OPEN_CREATE`.
- Existing empty paths are initialized only with `MYLITE_OPEN_CREATE`.
- Existing non-empty paths must already be MyLite directories.
- `mylite.meta`, `datadir/`, and `tmp/` survive clean close.
- `run/` is process-local and removed on clean close.
- `tmp/` is retained as a directory, but its contents are cleared on clean
  close.

## Embedded Lifecycle And API

The public API does not change. The slice only makes current result-code
behavior more precise for invalid directory inputs:

- `MYLITE_NOTFOUND` for missing or empty non-created database directories.
- `MYLITE_CORRUPT` for paths that look like incomplete or invalid MyLite
  directories.
- `MYLITE_IOERR` for filesystem errors while probing or creating the layout.

## Build, Size, And Dependencies

No new runtime dependencies or embedded profile changes are expected. The test
cleanup helpers use POSIX `nftw()` through existing embedded test targets only.

## Test Plan

1. Cover metadata file contents in open and close layout assertions.
2. Cover initializing a pre-existing empty directory with `MYLITE_OPEN_CREATE`.
3. Cover rejecting a pre-existing empty directory without `MYLITE_OPEN_CREATE`.
4. Cover rejecting non-empty directories without `mylite.meta`.
5. Cover rejecting invalid metadata and incomplete required layout.
6. Cover replacing stale `run/` state on open while preserving `run/` for a
   second live handle.
7. Run embedded and non-embedded build/test presets, format check, dev and
   embedded tidy, diff check, and size measurement.

## Acceptance Criteria

- Directory creation and adoption rules are explicit and tested.
- `mylite.meta` content and required durable subdirectories are validated.
- Invalid or incomplete database directories do not get silently repaired.
- Stale runtime state is cleaned on open without breaking multiple handles to
  the same active directory.
- Documentation and compatibility tables describe the enforced policy and
  remaining limits.

## Risks And Open Questions

- Cross-process stale `run/` detection is safe only after the locking slice
  takes the directory lock before replacing inactive `run/` state.
- Future metadata-format migration needs a dedicated slice before MyLite can
  open format versions other than `1`.
- Missing `datadir/` or `tmp/` currently means `MYLITE_CORRUPT`; a later repair
  tool could choose to recover some cases explicitly.
