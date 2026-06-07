# Ownerless CTest Runner Diagnostics

## Problem

Ownerless cross-process SQL coverage is split into sixteen deterministic CTest
shards. The embedded CTest presets run those shards serially unless a caller
supplies an explicit parallel override, so branch CI wall time can look worse
as ownerless evidence grows even when focused WordPress PHPUnit runtime stays
close to main.

The obvious performance experiment is to add preset-level CTest parallelism,
but ownerless SQL shards contain heavyweight cross-process DDL, dictionary,
temporary-tablespace, active-reader, and crash-recovery cases. A scheduling
change must prove it reduces wall time without creating load-sensitive hangs,
or it makes CI more noisy rather than faster.

## Source Findings

- `packages/libmylite/CMakeLists.txt` registers eight
  `libmylite.ownerless-cross-process-sql.<n>` tests, each invoking
  `mylite_ownerless_cross_process_sql_test sql-shard <n> 8`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` forks each SQL
  case into a fresh hidden child process.
- `.github/workflows/ci.yml` runs the embedded job through
  `ctest --preset php-embedded-dev`.
- CMake 3.31 documents test-preset `execution.jobs` as the preset equivalent
  of `ctest --parallel`.

## Design

Do not change the normal embedded or PHP-embedded CTest preset scheduling in
this slice. Validation showed that preset-level ownerless SQL parallelism is
not yet stable enough for CI.

Instead, harden the ownerless SQL runner so future performance experiments and
CI failures identify the exact work item:

- Store a stable name beside every ownerless SQL test function pointer.
- Print the active case name and index for case start, pass, child failure,
  wait failure, and timeout diagnostics.
- Provide `sql-case <index-or-name>` so a timeout can be reproduced directly
  through the same per-case fork, timeout, and cleanup wrapper used by shards.
- Start each hidden per-case child in its own process group.
- On a per-case timeout, send `SIGKILL` to the case process group and then to
  the direct child as a fallback.
- Drive the per-case timeout from a monotonic wall-clock deadline instead of a
  fixed number of poll sleeps, so scheduler delays cannot stretch the nominal
  timeout window.

## Rejected Scheduling Evidence

Four-job CTest parallelism was rejected:

- Shard 7 exposed an orphaned
  `test_ownerless_serializable_read_blocks_peer_update` hidden child that held
  CTest's output pipe after the shard parent was gone.
- After process-group cleanup was added, the full label passed once, but a
  later run timed out shard 6 in
  `test_concurrent_ownerless_ddl_allocates_unique_metadata`.
- Reserving all four slots for shard 6 made shard 6 pass alone, but shard 0
  then timed out in
  `test_ownerless_instant_column_variants_refresh_peer_dictionary`.

Two-job CTest parallelism with shard 6 reserved was also rejected:

- Direct shard 6 passed in about 66 seconds, including
  `test_concurrent_ownerless_ddl_allocates_unique_metadata` in about 4
  seconds.
- The full two-job label passed shards 0, 1, 2, 3, 4, 5, and 7, then ran shard
  6 alone as intended.
- Shard 6 then stalled in
  `test_ownerless_temporary_tablespace_allows_peer_temp_tables`; manual
  termination after more than 400 seconds proved the new diagnostics identified
  the exact active case, but the schedule was not acceptable to ship.

The result was a correctness decision: keep ordinary ownerless SQL CTest
scheduling serial until a later slice could split or weight the heavy cases
with passing evidence. The follow-up
`docs/specs/ownerless-sql-weighted-shards/specs.md` now switches registered
ownerless SQL shards from modulo assignment to deterministic estimated-weight
assignment. Its local ownerless SQL two-job measurement passed with about half
the serial ownerless SQL wall time, while global CI full-preset parallelism
remains evidence-gated.

## Compatibility Impact

No SQL, C API, storage, PHP, or wire behavior changes. This is test-runner
diagnostics and timeout cleanup only.

## Directory And Lifecycle Impact

No MyLite database directory behavior changes. Each CTest process continues to
use its own temporary database directory.

## Build And Performance Impact

The normal embedded presets intentionally remain serial for ownerless SQL
shards in this diagnostics slice. This avoided a measured CI regression where
attempted parallel scheduling produced load-sensitive ownerless timeouts and
longer failed runs. Weighted shard registration is covered separately by
`ownerless-sql-weighted-shards`, which records a passing two-job ownerless SQL
label measurement without changing CI full-preset scheduling.

The runner changes improve failure latency and triage quality: timeout cleanup
targets the whole hidden case process group, and named diagnostics remove the
normal-build versus hook-build case-index ambiguity when a shard times out or
fails.

## Test And Verification Plan

- Validate CTest registration with `ctest --preset embedded-dev -N`.
- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run focused serializable and online-DDL selectors directly in `embedded-dev`.
- Run direct shard 6 to prove the metadata-allocation case is not a
  deterministic functional failure.
- Run one hook-build ownerless selector directly.
- Run the ownerless SQL label through the serial `embedded-dev` preset.
- Run `format-check` and diff whitespace checks.

## Acceptance Criteria

- `ctest --preset embedded-dev -N` accepts the preset and lists the existing
  ownerless SQL shards.
- Focused ownerless selectors remain runnable directly and print stable case
  names.
- A named ownerless SQL case can be rerun with `sql-case <name>`.
- Direct shard 6 still passes.
- The ownerless SQL label passes under the unchanged serial embedded preset.
- A timed-out hidden ownerless SQL case is killed by process group, preventing
  orphaned descendants from holding CTest output pipes open.
- Compatibility docs describe the runner diagnostics accurately without
  claiming new product behavior or unproven parallel scheduling.
