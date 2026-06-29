# Ownerless Temporary Cross-Schema IF EXISTS Rename Coverage

## Problem Statement

Ownerless temporary/permanent `RENAME TABLE IF EXISTS` coverage proves
same-schema temp-first and permanent-first lists where missing sources are
skipped as warnings, a session-local temporary table is renamed, and a durable
permanent table moves. The remaining adjacent rename-list gap is a
cross-schema permanent move in the same statement. That shape matters because
MariaDB routes temporary source pairs outside the durable DDL log while the
permanent pair must move native `.frm`/`.ibd` files across schema directories
and keep the ownerless native file-operation marker on the durable lane.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc:279-329` implements `check_rename()`. Under
  `IF EXISTS`, missing durable source tables emit `ER_NO_SUCH_TABLE` notes and
  return `-1`, allowing the ordered rename loop to skip those pairs.
- `mariadb/sql/sql_rename.cc:506-674` implements `rename_tables()`. Temporary
  source pairs call `do_rename_temporary()` and are tracked only in a temporary
  revert list, while durable pairs call `check_rename()` and then `do_rename()`.
- `mariadb/sql/sql_rename.cc:395-405` records a successful durable table move
  with `mylite_ownerless_dictionary_native_file_op()` and fires the unsafe
  `rename-table-after-native-file-op` hook before MariaDB advances the DDL-log
  phase.
- `mariadb/sql/sql_table.cc:5562-5705` builds old/new schema-qualified native
  table paths for `mysql_rename_table()` and writes MariaDB's backup-log record
  only for non-temporary durable renames.

## Scope And Non-Goals

In scope:

- Add a production ownerless SQL selector for a mixed temporary/permanent
  `RENAME TABLE IF EXISTS` statement whose permanent table moves from `app` to
  `app_archive`.
- Add hook-build crash selectors for temp-first and permanent-first orderings
  killed at `dictionary-before-finish` while another ownerless process remains
  live.
- Verify warning/no-op behavior for missing sources in both schemas, temporary
  target non-durability, cross-schema durable target placement, native
  file-operation marker retention and drain, ownerless/native reopen, and
  forced `.shm` rebuild.

Out of scope:

- Native-loop rollback for this mixed temporary/permanent shape is covered by
  `docs/specs/ownerless-temporary-cross-schema-if-exists-rename-loop-coverage/specs.md`.
- Arbitrary randomized temporary/permanent permutations.
- Foreign-key mixed temporary/permanent rename lists.
- SQL-level local table-lock fault injection.

## Design

Use this deterministic statement in the production selector:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_tmp_cross_if_exists_missing_before
    TO app_archive.ownerless_tmp_cross_if_exists_missing_before_dst,
  app.ownerless_tmp_cross_if_exists_shadow
    TO app.ownerless_tmp_cross_if_exists_shadow_moved,
  app.ownerless_tmp_cross_if_exists_perm_src
    TO app_archive.ownerless_tmp_cross_if_exists_perm_dst,
  app_archive.ownerless_tmp_cross_if_exists_missing_after
    TO app.ownerless_tmp_cross_if_exists_missing_after_dst;
```

The session owns a temporary table named
`app.ownerless_tmp_cross_if_exists_shadow`, shadowing a durable table with the
same name. The temporary pair stays in `app`; the durable permanent pair moves
from `app` to `app_archive`; missing-source pairs exercise skipped-source
warnings on both sides of the cross-schema move.

Hook selectors reuse the same SQL shape and add a reverse ordering where the
durable permanent pair executes before the temporary pair. Both crash at the
existing ownerless dictionary finish boundary after MariaDB has completed the
statement but before MyLite publishes the ownerless dictionary boundary.

## Compatibility Impact

No production SQL semantics change. The slice adds evidence that MyLite
ownerless recovery preserves MariaDB's mixed temporary/permanent
`RENAME TABLE IF EXISTS` behavior when the durable pair moves across schemas:
missing sources produce notes, temporary targets are session-local, and durable
native table files end up only in the target schema.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format change. Tests verify that only the durable
permanent target has `.frm` and `.ibd` files in the target schema, skipped
targets are not materialized, temporary targets disappear when the crashed
session exits, and the native file-operation marker remains while a peer is
live before draining on final no-live recovery.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The new
hook selectors are test-only; the production selector exercises existing SQL
behavior.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors:
  `temporary-cross-schema-mixed-if-exists-rename`,
  `temporary-cross-schema-mixed-if-exists-rename-crash`, and
  `temporary-cross-schema-mixed-if-exists-rename-reverse-crash`.
- Run the focused hook CTest subset for temporary mixed rename coverage.
- Build the production embedded target and run the production cross-schema
  selector plus adjacent same-schema temporary mixed IF EXISTS selector.
- Run production-build guard, CI-style clang-format validation for touched C
  sources, `git diff --check`, and cleanup checks.

## Acceptance Criteria

- The production selector observes exactly two missing-source notes and keeps
  both skipped targets absent.
- The session-local temporary target is readable only before the writer closes;
  the durable shadow table remains under its original name afterward.
- The permanent source is absent from `app`, the permanent target exists in
  `app_archive`, and the target row is writable after recovery.
- Crash selectors keep the native file-operation marker set while a live peer
  remains open and drain it after final no-live recovery.
- Ownerless reopen, ordinary native reopen, and forced `.shm` rebuild all
  preserve the same durable state.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Direct hook selectors
  `temporary-cross-schema-mixed-if-exists-rename-crash` and
  `temporary-cross-schema-mixed-if-exists-rename-reverse-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-temporary-(mixed-if-exists-rename(-reverse)?|cross-schema-mixed-if-exists-rename(-reverse)?)-crash$' --output-on-failure`
  passed 4/4.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selectors `temporary-mixed-if-exists-rename` and
  `temporary-cross-schema-mixed-if-exists-rename` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed 1/1.
- Ubuntu 24.04 `clang-format --dry-run --Werror` passed for
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c`.
- `git diff --check` passed.
- Cleanup checks found no `mylite-ownerless-*` temp directories and no
  lingering MyLite or MariaDB test processes.

## Risks And Follow-Up

- This is a deterministic cross-schema mixed rename-list slice, not randomized
  temporary/permanent permutation coverage.
- Broader randomized DDL/RQG stress, FK mixed rename variants, and
  active-reader pressure breadth remain completion work.
