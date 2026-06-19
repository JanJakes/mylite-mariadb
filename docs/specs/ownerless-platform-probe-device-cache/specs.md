# Ownerless Platform Probe Device Cache

## Problem

The first ownerless open of a database directory without
`concurrency/mylite-ownerless-platform.meta` runs
`mylite_ownerless_probe_directory()`. The probe is correctness-sensitive
because ownerless mode requires same-directory `MAP_SHARED`, byte-range lock,
lock-release-on-exit, grow/remap, and wait-backend behavior. It is also
noticeable in process-startup profiles because it forks child probes and waits
on synchronization pipes.

A reduced production append-only sample reported the first ownerless
open/close at `455.324 ms`, including `65.805 ms` in
`open_platform_probe`. Later ownerless warm opens on the same database had
`open_platform_probe_ms_avg=0.031` because the directory proof file already
existed. Suites that create multiple fresh MyLite directories on the same
filesystem inside one process still pay the directory probe for each new
directory.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_probe.cc`
  `mylite_ownerless_probe_directory()` creates a temporary probe root under
  the target database directory, then checks shared mmap visibility,
  byte-range locks, lock release on child exit, grow/remap behavior, and the
  ownerless wait backend.
- `packages/libmylite/src/database.cc`
  `validate_ownerless_platform_for_database()` creates the `concurrency/`
  directory, accepts an existing `mylite-ownerless-platform.meta` proof only
  when its `database_device` matches `stat(database_path).st_dev`, otherwise
  runs the directory probe and writes a fresh proof file.
- Unsafe ownerless test hooks can force probe failures through
  `MYLITE_OWNERLESS_TEST_PROBE_FAIL`; negative coverage depends on those
  forced failures not being hidden by a process-local fast path.

## Design

Cache successful ownerless directory probe evidence per `st_dev` inside the
current process:

- existing per-directory proof metadata remains the first and strongest fast
  path;
- when a directory lacks proof metadata, a process-local matching device proof
  may skip the child-process probe;
- the opener still creates `concurrency/` and writes that directory's own
  `mylite-ownerless-platform.meta` before accepting ownerless mode;
- the process cache is populated from both existing matching directory proof
  metadata and successful directory probes;
- when unsafe probe-failure hooks are configured, the device cache is ignored
  so negative tests still exercise the real probe path.

The cache is process-local only. It does not create a global proof file outside
the MyLite database directory and does not change the durable proof format.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, DDL, storage-engine, or WAL behavior changes.
This reduces repeated ownerless startup probe work inside a process after a
device has already been proven.

## Directory And Lifecycle Impact

Each opened database directory still receives its own
`concurrency/mylite-ownerless-platform.meta` proof file before ownerless mode
is accepted. No durable state is written outside the database directory.

## Native Storage Impact

Native MariaDB/InnoDB files, page formats, redo, checkpoint, recovery, and
ownerless concurrency files are unchanged.

## Test And Verification Plan

- Add focused ownerless SQL hook coverage for the device cache and the
  unsafe-failure bypass.
- Build the ownerless SQL test under the hook preset.
- Run the focused platform-probe selector.
- Run the production embedded performance probe in reduced form and verify the
  first ownerless directory still writes proof metadata while a second
  same-device fresh directory reports a near-zero platform-probe phase.
- Run production build/static checks.

## Acceptance Criteria

- A successful directory probe can be reused for a second fresh database
  directory on the same device in the same process.
- The second directory still gets its own proof metadata.
- Unsafe forced probe failures are not masked by the device cache.
- No directory layout or persistent format changes are introduced.

## Verification

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Direct hook selector
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  platform-probe-failure` passed.
- Registered hook CTest
  `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-platform-probe-failure$' --output-on-failure`
  passed.
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- `ctest --preset embedded-prod -R
  '^libmylite\.(ownerless-primitives|ownerless-single-owner-history-wal-proof)$'
  --output-on-failure` passed.
- Reduced production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=10
  MYLITE_PERF_INSERT_ITERATIONS=20` passed and reported
  `mylite_perf_summary_ownerless_first_probe_open_close_open_platform_probe_ms_avg=211.855`
  for the first ownerless directory and
  `mylite_perf_summary_ownerless_device_cached_probe_open_close_open_platform_probe_ms_avg=0.047`
  for a second fresh same-device directory. The second open/close still pays
  cold runtime/InnoDB startup because it intentionally uses a separate runtime
  directory; the measured improvement is specifically the platform-probe phase.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/embedded-prod`, and
  `tools/require-cmake-release-build build/ownerless-test-hooks` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.
