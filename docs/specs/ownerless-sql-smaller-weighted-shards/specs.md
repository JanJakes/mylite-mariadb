# Ownerless SQL Smaller Weighted Shards

## Problem Statement

The ownerless SQL suite is already split into weighted CTest shards with
per-case child processes and a 300-second child watchdog. Current branch
verification still showed noisy shard behavior: under the eight-shard
registration, several cases timed out after many prior cases in a shard, while
the same cases passed directly in 4-5 seconds.

MyLite needs a bounded scheduling improvement that makes CI timings more
visible, lowers cumulative per-shard state, and keeps the existing per-case
watchdog strict instead of hiding hangs behind a larger timeout.

## Source Findings

- `packages/libmylite/CMakeLists.txt` currently registers
  `libmylite.ownerless-cross-process-sql.<n>` using
  `sql-weighted-shard <index> 8`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already allows
  up to `MYLITE_TEST_OWNERLESS_SQL_MAX_WEIGHTED_SHARDS`, currently 64.
- `run_ownerless_sql_test_case()` forks a fresh child for each case, puts the
  child in its own process group, and kills that process group when the
  300-second watchdog fires.
- Local verification while adding rename-create replay observed:
  - `ctest --preset php-embedded-dev -R ownerless-cross-process-sql --parallel 2`
    timed out in unrelated existing cases
    `test_ownerless_generated_column_index_ddl_refreshes_peer_dictionary` and
    `test_ownerless_serializable_read_blocks_peer_update`.
  - Both cases passed directly through `sql-case` in 5 seconds and 4 seconds.
  - A serial rerun of the eight-shard label later timed out in unrelated
    `test_ownerless_composite_foreign_keys_cross_process`, which then passed
    directly in 4 seconds.

## Design

Change the registered ownerless SQL shard count from 8 to 16.

This keeps the deterministic weighted assignment algorithm and the direct
`sql-weighted-shard <index> <count>` command unchanged. The smaller registered
shards reduce the number of prior cases each CTest test executes before a
given case, make per-shard timings more granular, and keep CTest output closer
to the actual slow case.

The per-case 300-second watchdog remains unchanged. If a single case truly
hangs, it should still fail quickly with the active case name and PID.

## Scope

In scope:

- CTest registration for normal ownerless SQL weighted shards.
- Compatibility and shard documentation updates.
- Verification of CTest discovery, focused direct selectors, and the registered
  ownerless SQL group.

Out of scope:

- Product ownerless SQL, storage, locking, page-version, redo, or recovery
  behavior.
- Increasing the per-case watchdog.
- Removing the old modulo `sql-shard` command.
- External MariaDB/RQG stress.

## Compatibility Impact

No MySQL/MariaDB compatibility behavior changes. This changes only regression
suite scheduling and CI timing visibility.

## Database Directory And Lifecycle Impact

No database-directory layout or runtime lifecycle changes. Each ownerless SQL
case still creates and removes its own temporary MyLite database directory
through existing helpers.

## Native Storage Impact

No native storage format or runtime behavior changes.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The CTest graph gains eight additional ownerless SQL test entries.

## Test Plan

- Reconfigure/build `mylite_ownerless_cross_process_sql_test` in
  `php-embedded-dev`.
- Validate CTest discovery reports 16 ownerless SQL shard tests.
- Run a direct weighted shard with the new count.
- Run focused direct cases that timed out under the eight-shard wrapper.
- Run the registered ownerless SQL CTest group under the CI-shaped
  `--parallel 2` ownerless invocation.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- CTest discovers 16 `libmylite.ownerless-cross-process-sql.<n>` tests with
  the existing `compat.ownerless-cross-process-sql` label.
- Registered ownerless SQL tests invoke `sql-weighted-shard <index> 16`.
- `sql-case <index-or-name>` and the old modulo `sql-shard` command remain
  available.
- The per-case watchdog remains 300 seconds.
- Focused previously timed-out cases still pass directly.
- The registered ownerless SQL group has clearer per-shard timing evidence;
  any remaining timeout is recorded with exact case identity rather than being
  hidden as a monolithic suite failure.

## Evidence

CTest discovery reports 16 ownerless SQL weighted shards:

```text
ctest --preset php-embedded-dev -N -R ownerless-cross-process-sql
Total Tests: 16
```

Verbose CTest discovery shows the registered command uses the smaller shard
count:

```text
mylite_ownerless_cross_process_sql_test sql-weighted-shard 0 16
mylite_ownerless_cross_process_sql_test sql-weighted-shard 15 16
```

Direct weighted shard 0 passed with a lower estimated shard weight than the
old eight-shard weights:

```text
ownerless-sql weighted-shard start index=0 count=16 cases=160 weight=36
ownerless-sql weighted-shard pass index=0 count=16 weight=36
```

The three cases that timed out under the previous eight-shard wrapper still
passed directly:

```text
test_ownerless_generated_column_index_ddl_refreshes_peer_dictionary seconds=4
test_ownerless_serializable_read_blocks_peer_update seconds=3
test_ownerless_composite_foreign_keys_cross_process seconds=4
```

The registered ownerless SQL group passed under the CI-shaped two-job
invocation:

```text
ctest --preset php-embedded-dev -R ownerless-cross-process-sql --parallel 2 --output-on-failure
100% tests passed, 0 tests failed out of 16
Total Test time (real) = 329.84 sec
```

## Risks And Open Questions

- Smaller shards reduce cumulative state and improve visibility, but they do
  not prove the underlying intermittent ownerless SQL hangs are impossible.
- If CI still sees direct-case timeouts, the next slice should gather in-child
  diagnostics at the active SQL wait boundary rather than further weakening the
  test harness.

## Follow-Up CI Adoption

`docs/specs/ci-ownerless-sql-weighted-shards/specs.md` records the follow-up
CI change that moved the production `ubuntu-embedded` ownerless SQL step from a
serial direct `sql-case` loop to the registered sixteen-shard CTest selector:

```text
ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 2 --output-on-failure
```

That follow-up changes CI scheduling and timing visibility only; the shard
count, weighted assignment, per-case child wrapper, and 300-second watchdog
remain the behavior from this slice.
