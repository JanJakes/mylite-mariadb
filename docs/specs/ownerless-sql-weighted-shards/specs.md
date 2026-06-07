# Ownerless SQL Weighted Shards

## Problem Statement

Ownerless cross-process SQL coverage is registered as eight CTest shards, but
the current modulo assignment puts case `i` into shard `i % 8`. That is stable
and simple, but it does not account for heavier DDL, pressure, BLOB,
foreign-key, temporary-table, or crash-recovery cases. A local rerun of shard
0 after adding cross-schema multi-drop replay passed the new case quickly but
then intermittently timed out in an existing view-security case that passes
directly in about five seconds. This matches the current CI pain: broad
ownerless shards can be noisy and expensive even when individual cases are not
deterministically broken.

MyLite needs a bounded test-harness improvement that evens out shard cost and
creates measurable parallel-scheduling input without changing product SQL,
storage, or ownerless runtime behavior.

## Source Findings

- `packages/libmylite/CMakeLists.txt` registers eight
  `libmylite.ownerless-cross-process-sql.<n>` tests under the
  `compat.ownerless-cross-process-sql` label.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` stores stable
  per-case names in `ownerless_sql_test_cases[]`, runs each shard through a
  per-case child process, and supports direct `sql-case <index-or-name>`
  replay through the same child-runner wrapper.
- `docs/specs/ownerless-ctest-parallel-scheduling/specs.md` records rejected
  two-job and four-job modulo-shard parallelism. The rejection was not a SQL
  semantics decision; it was a test scheduling decision after load-sensitive
  timeouts.
- `docs/specs/ownerless-sql-ctest-shards/specs.md` records the original
  modulo-shard registration and the per-case diagnostics.

## Design

Keep the existing `sql-shard <index> <count>` command for manual modulo-shard
comparison, but add `sql-weighted-shard <index> <count>` and switch CTest
registration to that command.

Weighted assignment is deterministic and local to the test harness:

1. Each ownerless SQL case receives an estimated cost from its stable case
   name. Heavier families include stress, pressure, BLOB/compressed BLOB,
   DDL/dictionary, view/trigger, foreign-key, index/key, generated-column,
   schema, and crash/recovery names.
2. The runner greedily assigns cases in table order to the currently lightest
   shard by estimated cumulative weight, breaking ties by lower shard index.
3. Each weighted shard runs its assigned cases through the same
   `run_ownerless_sql_test_case()` child wrapper as modulo shards.
4. Shard diagnostics print the estimated total weight for the selected shard,
   so a timeout log identifies both active case and predicted shard size.

This does not enable global CTest parallelism by itself. The first acceptance
target is balanced deterministic registration with passing focused and label
evidence. Parallel CTest jobs can then be measured explicitly instead of
assuming the old modulo-shard behavior.

## Scope

In scope:

- Test-harness scheduling logic for normal and hook builds.
- CTest registration for the normal ownerless SQL shards.
- Documentation updates for compatibility and prior shard specs.
- Verification of direct weighted shard execution, CTest discovery, focused
  ownerless cases, and measured safe parallel ownerless label behavior when
  it passes.

Out of scope:

- Product ownerless SQL, storage, locking, page-version, redo, or recovery
  behavior.
- Changing CI to use global `ctest --parallel` before weighted shards have
  passing evidence.
- Removing the old modulo `sql-shard` command.
- SQL-level table-lock fault injection.

## Compatibility Impact

No MySQL/MariaDB compatibility behavior changes. This slice changes only how
the ownerless SQL regression suite is partitioned for CTest.

## Database Directory And Lifecycle Impact

No database-directory layout or lifecycle changes. Each case still creates and
removes its own temporary MyLite database directory through the existing test
helpers. The per-case child wrapper, timeout, and process-group cleanup are
unchanged.

## Native Storage Impact

No native storage format or runtime behavior changes. The same ownerless SQL
cases continue to exercise native InnoDB storage through existing code paths.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The change is limited to tests, CTest registration, and docs.

## Test Plan

- Configure/build `embedded-dev` after the CMake registration change.
- Validate CTest discovery with `ctest --preset embedded-dev -N -R
  'ownerless-cross-process-sql'`.
- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev` and
  `ownerless-test-hooks`.
- Run direct `sql-weighted-shard 0 8` and one direct `sql-case` known to have
  timed out intermittently under modulo shard state.
- Run the ownerless SQL label under the serial embedded preset.
- Run a measured two-job ownerless SQL label only if the weighted serial label
  passes; do not turn on CI parallelism unless the evidence is stable.
- Run `format-check`, `git diff --check`, cached diff checks, and cleanup
  checks.

## Acceptance Criteria

- CTest still discovers eight `libmylite.ownerless-cross-process-sql.<n>`
  tests with the same label.
- The registered tests invoke `sql-weighted-shard`.
- The old `sql-shard` command remains available for modulo comparison.
- `sql-case <index-or-name>` behavior remains unchanged.
- Weighted shard diagnostics include shard index, count, case count, and
  estimated shard weight.
- Focused weighted shard and isolated previously slow case runs pass.
- Serial ownerless SQL CTest label passes.
- Any attempted parallel ownerless SQL CTest run is reported with exact
  command, timing, and failure evidence if it is still not safe.

## Evidence

CTest discovery still reports eight ownerless SQL tests:

```text
ctest --preset embedded-dev -N -R 'ownerless-cross-process-sql'
```

Verbose CTest discovery shows the registered command switched to
`sql-weighted-shard`:

```text
libmylite.ownerless-cross-process-sql.0
  mylite_ownerless_cross_process_sql_test sql-weighted-shard 0 8
```

Direct weighted shard 0 passed and printed its estimated shard weight:

```text
ownerless-sql weighted-shard start index=0 count=8 cases=156 weight=64
ownerless-sql weighted-shard pass index=0 count=8 weight=64
```

The full serial embedded ownerless SQL label passed:

```text
ctest --preset embedded-dev -L compat.ownerless-cross-process-sql --output-on-failure
8/8 tests passed
Total Test time (real) = 609.47 sec
```

The measured two-job ownerless SQL label passed with nearly half the wall time:

```text
ctest --preset embedded-dev -L compat.ownerless-cross-process-sql -j2 --output-on-failure
8/8 tests passed
Total Test time (real) = 312.45 sec
```

The weighted shard starts from that run reported estimated shard weights:

```text
index=0 weight=64
index=1 weight=63
index=2 weight=67
index=3 weight=66
index=4 weight=63
index=5 weight=63
index=6 weight=65
index=7 weight=63
```

## Risks And Open Questions

- Name-derived weights are estimates, not a substitute for measured historical
  timing data. They are intentionally conservative and easy to adjust after
  CI evidence accumulates.
- Weighted shards reduce predicted imbalance but do not remove all shared
  CPU, I/O, or MariaDB embedded startup contention. Global CTest parallelism
  remains a separate decision that must be proved before CI adopts it.
