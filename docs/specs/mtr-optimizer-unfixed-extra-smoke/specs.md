# MTR Optimizer Unfixed Extra Smoke

## Goal

Promote passing `optimizer_unfixed_bugs` MTR rows for the `bug41996-extra`
family under the embedded smoke profile:

- `optimizer_unfixed_bugs.bug41996-extra1`
- `optimizer_unfixed_bugs.bug41996-extra1-innodb`
- `optimizer_unfixed_bugs.bug41996-extra2`
- `optimizer_unfixed_bugs.bug41996-extra2-innodb`
- `optimizer_unfixed_bugs.bug41996-extra3`
- `optimizer_unfixed_bugs.bug41996-extra3-innodb`
- `optimizer_unfixed_bugs.bug41996-extra4`
- `optimizer_unfixed_bugs.bug41996-extra4-innodb`

## Non-Goals

- Broad optimizer-unfixed-suite coverage.
- Reclassifying native-engine failures that should remain future routed-engine
  compatibility candidates.
- Running these rows against MyLite storage-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- The selected `bug41996-extra*` rows pass under the MyLite embedded MTR smoke
  profile without upstream source changes.
- The `-innodb` variants also pass because they do not require native InnoDB
  bootstrap under this profile.
- `optimizer_unfixed_bugs.bug45219` failed on explicit native
  `ENGINE=MyISAM`; that remains unclassified because MyLite routes
  application `ENGINE=MyISAM` metadata to MyLite storage and should prove that
  behavior through routed-engine compatibility instead of treating the SQL
  shape as out of scope.

## Compatibility Impact

The opt-in embedded MTR smoke runner now covers additional optimizer regression
rows. This is MariaDB embedded baseline coverage only and does not change SQL,
C API, storage-engine, file-format, or server-surface policy.

## Design

- Add the selected passing tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Do not modify upstream MariaDB test files.
- Keep the native MyISAM failure outside accepted coverage and outside known
  unsupported classification.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice expands opt-in MariaDB embedded MTR
baseline coverage only.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe optimizer_unfixed_bugs.bug41996-extra1 optimizer_unfixed_bugs.bug41996-extra1-innodb optimizer_unfixed_bugs.bug41996-extra2 optimizer_unfixed_bugs.bug41996-extra2-innodb optimizer_unfixed_bugs.bug41996-extra3 optimizer_unfixed_bugs.bug41996-extra3-innodb optimizer_unfixed_bugs.bug41996-extra4 optimizer_unfixed_bugs.bug41996-extra4-innodb optimizer_unfixed_bugs.bug45219`
- `tools/mylite-mtr-harness run optimizer_unfixed_bugs.bug41996-extra1 optimizer_unfixed_bugs.bug41996-extra1-innodb optimizer_unfixed_bugs.bug41996-extra2 optimizer_unfixed_bugs.bug41996-extra2-innodb optimizer_unfixed_bugs.bug41996-extra3 optimizer_unfixed_bugs.bug41996-extra3-innodb optimizer_unfixed_bugs.bug41996-extra4 optimizer_unfixed_bugs.bug41996-extra4-innodb`
- `tools/mylite-mtr-harness coverage`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the eight passing optimizer rows.
- All selected tests pass under the MyLite MTR smoke profile.
- Coverage inventory counts increase accepted upstream baseline coverage by
  eight files without changing known unsupported counts.
- No upstream MariaDB test files are modified for this slice.

## Verification Results

- `tools/mylite-mtr-harness probe optimizer_unfixed_bugs.bug41996-extra1 optimizer_unfixed_bugs.bug41996-extra1-innodb optimizer_unfixed_bugs.bug41996-extra2 optimizer_unfixed_bugs.bug41996-extra2-innodb optimizer_unfixed_bugs.bug41996-extra3 optimizer_unfixed_bugs.bug41996-extra3-innodb optimizer_unfixed_bugs.bug41996-extra4 optimizer_unfixed_bugs.bug41996-extra4-innodb optimizer_unfixed_bugs.bug45219`: 8 passed, 1 failed, and 0 skipped.
- `tools/mylite-mtr-harness run optimizer_unfixed_bugs.bug41996-extra1 optimizer_unfixed_bugs.bug41996-extra1-innodb optimizer_unfixed_bugs.bug41996-extra2 optimizer_unfixed_bugs.bug41996-extra2-innodb optimizer_unfixed_bugs.bug41996-extra3 optimizer_unfixed_bugs.bug41996-extra3-innodb optimizer_unfixed_bugs.bug41996-extra4 optimizer_unfixed_bugs.bug41996-extra4-innodb`: passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 440 accepted
  upstream baseline tests, 8 accepted MyLite profile tests, 19 accepted MyLite
  storage-routed tests, 467 accepted total tests, 4,617 known unsupported
  upstream tests, and 844 unclassified upstream tests.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`: passed.

## Risks And Open Questions

- The remaining optimizer-unfixed rows need separate compatibility review.
- Native-engine failures in this suite may become storage-routed coverage rather
  than unsupported baseline rows.
