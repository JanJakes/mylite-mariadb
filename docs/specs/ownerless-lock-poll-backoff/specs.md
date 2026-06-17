# Ownerless Lock Poll Backoff

## Problem

Ownerless SQL statement gates and startup/directory locks use POSIX file locks.
The current wait loop retries `F_SETLK`/`flock(... LOCK_NB)` after a fixed
10 ms sleep. That keeps the implementation portable and bounded, but it adds
avoidable latency when a peer releases a short-lived ownerless statement lock
just after a waiter goes to sleep. The checksum stress retry slice exposed this
same class of short ownerless statement-lock handoff as a noisy performance and
CI signal.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` routes ownerless statement-lock waits
  through `acquire_fd_range_lock()`, using `F_SETLK` on byte ranges inside
  `mylite-statements.lock`.
- The same file routes ordinary database-directory lock waits through
  `wait_for_database_lock()`, using `flock(... LOCK_EX | LOCK_NB)`.
- Both loops previously slept for the fixed `k_lock_poll_interval_ms = 10`
  after every compatible transient lock conflict.
- The wait-timeout contract remains owned by MyLite: ownerless statement locks
  default to 60 seconds, while successful `SET lock_wait_timeout = N` maps to
  the ownerless wait budget for that handle.

## Design

Use adaptive polling for contended directory-owned file locks:

- First retry waits 1 ms.
- Each later retry doubles the interval.
- The interval caps at the previous 10 ms fixed poll.
- Timeout decisions still use the existing monotonic-clock deadline checks.
- Zero-timeout calls remain fail-fast because they return before sleeping.

This targets short ownerless statement-lock and startup-lock handoffs while
leaving lock compatibility, lock ownership, file layout, diagnostics, and
timeout values unchanged.

## Scope And Non-Goals

In scope:

- `acquire_fd_range_lock()` wait-loop latency for ownerless statement gates and
  other directory-owned byte-range locks.
- `wait_for_database_lock()` wait-loop latency for ordinary database-directory
  lock handoff.
- Documentation of the wait policy.

Out of scope:

- Replacing POSIX file locks or adding platform-specific timed blocking locks.
- Changing ownerless statement-lock compatibility classes.
- Changing `lock_wait_timeout`, `busy_timeout_ms`, or diagnostics.
- Solving same-table writer fairness beyond reducing short handoff latency.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage-format, or directory-layout
behavior changes. Contended waits retry sooner at first but still honor the
same lock modes and timeout budget.

## Database Directory And Lifecycle Impact

No durable layout changes. The slice only changes process-local wait cadence
while acquiring existing directory-owned lock files.

## Native Storage Impact

Native MariaDB/InnoDB locking, recovery, and data files are unchanged.

## Build, Size, And Dependency Impact

No new dependency and no meaningful binary-size impact. The implementation
reuses existing `<chrono>` and `<thread>` usage.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run direct `tx-stress` before and after the change to ensure concurrent
  ownerless transaction stress still passes and to capture a rough handoff
  timing sample.
- Run focused ownerless statement/checkpoint selectors that exercise
  directory-owned statement gates and no-live cleanup paths.
- Run the focused ownerless checksum stress from the production stress preset.
- Run production-build guards, format check, and whitespace check.

## Verification Results

Local verification on 2026-06-17 used the production `MinSizeRel` MariaDB
embedded archive and first-party `Release` builds:

- Baseline direct `tx-stress` before the change: `real 5.71` seconds.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- Direct `tx-stress` after the change passed three times with `real 6.28`,
  `5.38`, and `5.83` seconds. The sample is noisy and should be treated as
  neutral rather than a proven throughput win.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-stress -R
  libmylite.ownerless-cross-process-checksum-stress --output-on-failure`
  passed in `55.09` seconds.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` and
  `build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  passed. The probe reported ownerless active-runtime reconnect at `0.815` ms
  average, ownerless direct-select ratio `0.9669`, ownerless prepared-select
  ratio `0.7646`, ownerless explicit insert ratio `0.3594`, and ownerless
  autocommit insert ratio `0.6411`.
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
  passed.
- `build/embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `ctest --preset embedded-prod -R
  'libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure` passed.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Positive-timeout file-lock waits use 1, 2, 4, 8, and then 10 ms polling.
- Zero-timeout waits remain fail-fast.
- Existing ownerless cross-process SQL and stress coverage still passes.
- Docs and compatibility notes describe the behavior without claiming broader
  same-table writer fairness is solved.

## Risks And Unresolved Questions

- Adaptive polling reduces handoff latency but does not guarantee FIFO fairness
  for same-table ownerless writers. If same-table contention remains visible in
  application workloads, a separate fairness design is still needed.
- Local timing samples are noisy; this slice uses correctness/stability tests
  plus before/after rough stress timings rather than a hard performance gate.
