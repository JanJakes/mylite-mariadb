# Ownerless SQL Shard 8 Pressure Isolation

## Problem

The production ownerless SQL CI job runs sixteen weighted CTest shards with
`--parallel 2` so the broad ownerless DDL/concurrency matrix remains visible
without reverting to a monolithic test. Run `27864786059` passed every job
except the first `ubuntu-embedded` attempt, where
`libmylite.ownerless-cross-process-sql.8` timed out inside
`test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`.

The same direct case and the same weighted shard passed locally after the
failure, and the failed CI job passed on rerun. This matches the existing
load-sensitive pattern already documented for shard 11: the case is quick
alone, but the ownerless DDL/dictionary path can exceed the per-case watchdog
when two ownerless SQL shards apply sustained embedded-runtime pressure.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/CMakeLists.txt` registers
  `libmylite.ownerless-cross-process-sql.<index>` as weighted ownerless SQL
  CTest shards and already marks shard 11 `RUN_SERIAL` for a similar
  load-sensitive DDL timeout.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` runs each
  ownerless SQL case in a hidden child with a 300-second per-case watchdog,
  emits the active case name/index on timeout, and exposes
  `sql-case <index-or-name>` plus `sql-weighted-shard <index> <count>` for
  isolated reproduction.
- CI run `27864786059` timed out shard 8 at
  `test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`.
  Local reproduction of that exact `sql-case` passed in about 3 seconds, and
  local reproduction of `sql-weighted-shard 8 16` passed in under 30 seconds.
  The failed CI job passed on rerun.

## Scope And Non-Goals

In scope:

- Mark `libmylite.ownerless-cross-process-sql.8` as `RUN_SERIAL` so CTest does
  not run it concurrently with another ownerless SQL shard under CI pressure.
- Keep the same shard command, test cases, per-case watchdog, and failure
  diagnostics.
- Document the observed reason and reproduction evidence.

Out of scope:

- Removing or skipping any ownerless SQL coverage.
- Increasing the per-case watchdog.
- Reworking weighted-shard assignment.
- Changing ownerless runtime behavior, SQL semantics, or native storage paths.

## Design

The CTest registration now treats shard 8 the same way as the existing shard 11
exception: the shard still runs as part of the ownerless SQL label, but CTest
serializes it with other tests. This trades a small amount of parallelism for a
more stable production correctness signal and avoids burning CI time on
rerunning transient load-sensitive timeouts.

## Compatibility Impact

No SQL or API behavior changes. This is test-scheduling evidence only; it keeps
the ownerless SQL compatibility matrix intact.

## Directory And Lifecycle Impact

No directory layout, durable file, or embedded-runtime lifecycle changes.

## Native Storage Impact

No production native-storage changes.

## Public API Impact

No public API changes.

## Binary Size Impact

No binary changes.

## Test Plan

- Reconfigure or use an existing `php-embedded-prod` build directory.
- Verify CTest registration for
  `libmylite.ownerless-cross-process-sql.8`.
- Run the exact failed direct case:
  `mylite_ownerless_cross_process_sql_test sql-case
  test_ownerless_text_blob_prefix_index_ddl_refreshes_peer_dictionary`.
- Run the exact weighted shard:
  `ctest --preset php-embedded-prod -R
  '^libmylite.ownerless-cross-process-sql.8$' --output-on-failure`.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Shard 8 remains registered under the ownerless SQL CTest label.
- Shard 8 has the `RUN_SERIAL` property.
- The direct case and shard still pass locally.
- CI rerun passes without weakening ownerless SQL coverage.

## Risks And Follow-Up

- Serializing shard 8 can add some wall time when CTest would otherwise run it
  in parallel, but it should reduce rerun churn from load-sensitive timeouts.
- If other shards show repeated timeout patterns, they need the same evidence:
  failing CI case identity, isolated direct case result, isolated shard result,
  and rerun behavior before changing scheduling.
