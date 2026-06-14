# Ownerless Tablespace Ambiguous Replay

## Problem

Ownerless no-live recovery and active-reader boundary synthesis resolve native
InnoDB files by scanning the MyLite datadir for a page-0 FSP header whose
`SPACE` matches the retained page-version record. That is the right bridge for
covered rename, recreate, truncate, force-rebuild, and schema-drop lifecycles,
but the bridge must not guess if a directory contains two native files that
both claim the same tablespace id.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines the native FIL header
  offsets used by the bridge: `FIL_PAGE_OFFSET`, `FIL_PAGE_LSN`,
  `FIL_PAGE_TYPE`, and `FIL_PAGE_SPACE_ID`.
- The same header defines `FIL_PAGE_TYPE_FSP_HDR = 8` as the tablespace-header
  page type.
- `mariadb/storage/innobase/fsp/fsp0fsp.cc` initializes page 0 by writing
  `FIL_PAGE_TYPE_FSP_HDR` and the InnoDB space id into the FSP header.
- `packages/libmylite/src/ownerless_tablespace_replay.cc` resolves existing
  tablespaces by recursively scanning the database `datadir` for exactly one
  regular file whose page 0 matches the target space id and page size.

## Scope And Non-Goals

In scope:

- Prove strict primitive replay fails closed when two valid page-0 FSP-header
  candidates claim the same space id.
- Prove product replay's missing-tablespace skip mode treats that ambiguity as
  unresolved and does not mutate either file.
- Prove native boundary reads return `NOT_FOUND` instead of choosing one
  candidate.
- Document the behavior as part of the conservative native-file bridge.

Out of scope:

- Reconstructing missing DDL-created tablespaces.
- Choosing between duplicate native files by SQL dictionary metadata, filename,
  modification time, or page LSN.
- Changing the page-version WAL format, checkpoint protocol, or native InnoDB
  file layout.
- Claiming the broader DDL/file-lifecycle recovery bucket complete.

## Design

The existing resolver already requires a unique page-0 match. This slice makes
that rule explicit and covered:

- the recursive datadir scan ignores regular files whose page 0 is not an FSP
  header for the target `SPACE`;
- the first matching FSP-header candidate becomes the tentative target;
- a second matching candidate makes the scan fail;
- strict replay reports an error because the retained page cannot be placed
  into a unique native file;
- product no-live replay with
  `MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES` skips the
  unresolved record and leaves both native files unchanged;
- active-reader boundary reads report `NOT_FOUND`, so boundary synthesis stays
  conservative.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, or native file-format change.
The visible behavior is a stricter recovery proof: MyLite refuses to select a
native target when the directory itself provides contradictory page-0 space-id
evidence.

## Directory And Lifecycle Impact

No new files or directory layout changes. The test creates two synthetic schema
subdirectories under a temporary datadir only to exercise the recursive scan.

## Native Storage Impact

No native InnoDB page format changes. The bridge still relies on InnoDB page-0
FSP-header identity and remains conservative for broader DDL/file lifecycle
cases.

## Test And Verification Plan

- Extend `mylite_ownerless_primitives_test` with duplicate-FSP-header
  tablespace replay coverage.
- Build `mylite_ownerless_primitives_test` with `php-embedded-prod`.
- Run `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`.
- Run focused ownerless SQL file-lifecycle replay selectors.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Strict primitive replay returns `MYLITE_OWNERLESS_TABLESPACE_REPLAY_ERROR`
  when two `.ibd` candidates have the same page-0 FSP `SPACE`.
- Product missing/unresolved replay mode returns OK without mutating either
  ambiguous file.
- Native boundary reads return
  `MYLITE_OWNERLESS_TABLESPACE_REPLAY_NOT_FOUND` for the same ambiguity.
- Compatibility docs record this as bounded evidence while broader native
  DDL/file-lifecycle recovery remains partial.

## Implementation Evidence

- `test_tablespace_replay_rejects_ambiguous_tablespace()` creates two schema
  subdirectory `.ibd` candidates with the same page-0 FSP `SPACE`, appends a
  retained page-version record for that space, and verifies strict replay,
  product skip mode, and boundary-read behavior.
- `ownerless_tablespace_replay.cc` now documents the fail-closed duplicate
  candidate rule at the resolver scan site.
- Production `mylite_ownerless_primitives_test` build passed.
- Production `libmylite.ownerless-primitives` CTest passed.
- Direct ownerless SQL file-lifecycle replay cases passed for dropped,
  same-schema multi-dropped, cross-schema multi-dropped, renamed,
  rename-create, truncated, schema-dropped, force-rebuilt, multi-renamed,
  created, recreated, `CREATE OR REPLACE`, `CREATE OR REPLACE ... AS SELECT`,
  and `CREATE OR REPLACE ... LIKE` tablespaces.
- `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check` passed.

## Risks And Follow-Up

- This proves the bridge avoids guessing, but it does not recover ambiguous
  native-file states. A fuller DDL/file-lifecycle protocol still needs SQL
  dictionary/file-operation evidence for cases where files are missing,
  duplicated, or otherwise inconsistent after interrupted native DDL.
- External MariaDB/RQG-style stress remains useful to discover additional DDL
  lifecycle shapes, but this slice deliberately keeps the local regression
  small and deterministic.
