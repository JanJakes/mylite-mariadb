# Ownerless Temporary Cross-Schema IF EXISTS Rename Loop Coverage

## Problem Statement

Cross-schema mixed temporary/permanent `RENAME TABLE IF EXISTS` prefinish
coverage proves the completed-statement ownerless dictionary boundary. It does
not prove the deeper native-loop crash point after MariaDB has moved the
durable permanent table across schema directories but before the DDL-log phase
advances. At that point MariaDB recovery must roll the permanent table back to
its original source name, while the session-local temporary rename disappears
with the crashed writer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:506-674` implements the ordered rename loop.
  Temporary pairs call `do_rename_temporary()` and are only kept in a temporary
  revert list; durable pairs call `check_rename()` and `do_rename()`.
- `mariadb/sql/sql_rename.cc:395-405` records each successful durable native
  rename through `mylite_ownerless_dictionary_native_file_op()` and fires the
  unsafe `rename-table-after-native-file-op` hook before MariaDB advances the
  table-rename DDL-log phase.
- `mariadb/sql/sql_table.cc:5562-5705` builds schema-qualified old/new native
  paths for `mysql_rename_table()` and writes backup-log records for
  non-temporary durable renames.

## Scope And Non-Goals

In scope:

- Add hook-build crash selectors for temp-first and permanent-first
  cross-schema mixed temporary/permanent `RENAME TABLE IF EXISTS` lists killed
  at `rename-table-after-native-file-op`.
- Verify DDL-log rollback restores the permanent source table in `app` with
  its original InnoDB `SPACE` id.
- Verify the cross-schema permanent target, temporary target, and skipped
  missing targets remain absent after recovery.
- Verify live-peer marker retention, final no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Arbitrary skip counts. This statement has one durable native file operation,
  so the first hook hit is the only native-loop boundary.
- Randomized temporary/permanent permutations.
- Foreign-key mixed temporary/permanent rename lists.
- SQL-level local table-lock fault injection.

## Design

Reuse the cross-schema mixed `RENAME TABLE IF EXISTS` SQL from the prefinish
slice and arm `rename-table-after-native-file-op` instead of
`dictionary-before-finish`. The no-live recovery oracle differs from completed
prefinish recovery:

- `app.ownerless_tmp_cross_if_exists_perm_src` must be present and writable;
- `app_archive.ownerless_tmp_cross_if_exists_perm_dst` must be absent;
- `app.ownerless_tmp_cross_if_exists_shadow` must remain as the durable
  shadow table;
- `app.ownerless_tmp_cross_if_exists_shadow_moved` must be absent because the
  temporary table belonged to the killed session;
- skipped missing targets must be absent in both schemas.

## Compatibility Impact

No production SQL behavior changes. The slice adds evidence that ownerless
recovery preserves MariaDB DDL-log rollback for the durable portion of a mixed
temporary/permanent `RENAME TABLE IF EXISTS` list.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format change. Tests verify that `.frm`/`.ibd`
files for the permanent source are restored under `datadir/app/`, no permanent
target files remain under `datadir/app_archive/`, temporary files are not
materialized, and ownerless native file-operation markers drain only after the
final live peer exits.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The new
selectors are hook-build tests only.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors
  `temporary-cross-schema-mixed-if-exists-rename-loop-crash` and
  `temporary-cross-schema-mixed-if-exists-rename-reverse-loop-crash`.
- Run the focused hook CTest subset for cross-schema mixed IF EXISTS
  prefinish and native-loop coverage.
- Build the production embedded target and run the production cross-schema
  mixed IF EXISTS selector to prove unsafe hooks compile out.
- Run production-build guard, CI-style clang-format validation for touched C
  sources, `git diff --check`, and cleanup checks.

## Acceptance Criteria

- Both native-loop crash selectors recover the permanent source table in `app`
  with the original `SPACE` id.
- The cross-schema permanent target, skipped missing targets, and temporary
  target are absent after recovery.
- A peer can write the recovered source table while the native file-operation
  marker remains retained by a live peer.
- Final no-live recovery drains the marker.
- Ownerless reopen, ordinary native reopen, and forced `.shm` rebuild preserve
  the recovered source state.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Direct hook selectors
  `temporary-cross-schema-mixed-if-exists-rename-loop-crash` and
  `temporary-cross-schema-mixed-if-exists-rename-reverse-loop-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-temporary-cross-schema-mixed-if-exists-rename(-reverse)?(-loop)?-crash$' --output-on-failure`
  passed 4/4.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selector `temporary-cross-schema-mixed-if-exists-rename` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed 1/1.
- Ubuntu 24.04 `clang-format --dry-run --Werror` passed for
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c`.
- `git diff --check` passed.
- Cleanup checks found no `mylite-ownerless-*` temp directories and no
  lingering MyLite or MariaDB test processes.

## Risks And Follow-Up

- This closes one deterministic durable native-loop boundary for the mixed
  cross-schema `IF EXISTS` list. Randomized mixed temporary/permanent rename
  permutations, FK mixed rename variants, broader DDL file lifecycle, and
  external MariaDB/RQG stress remain completion work.
