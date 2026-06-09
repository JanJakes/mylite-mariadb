# Embedded Repeated-Open Redo Repair

## Problem

The production embedded performance probe intermittently aborts during its
ordinary warm open/close loop, before ownerless write timing begins. The failure
shape is:

```text
InnoDB: Invalid log header checksum
InnoDB: Plugin initialization aborted with error Data structure corruption
Unknown/unsupported storage engine: InnoDB
```

The reduced ownerless attribution probe usually uses
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, so it can miss this repeated-startup
failure. CI timing uses production builds and a default repeated open/close
sample, so this instability can make timing jobs look random and can hide the
actual engine/per-process startup costs.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0recv.cc:1736` starts
  `recv_sys_t::find_checkpoint()`, opens `ib_logfile0`, reads the redo header,
  and reports `InnoDB: Invalid log header checksum` when
  `recv_check_log_block()` fails on the header block.
- `packages/libmylite/src/database.cc` already has a validated redo startup
  prefix snapshot path:
  `capture_ownerless_redo_startup_prefix()`,
  `ownerless_redo_prefix_has_valid_current_checkpoint()`, and
  `restore_ownerless_redo_shutdown_header_if_needed()`.
- That repair is currently used only for no-live ownerless shutdown after
  taking `mylite-runtime-startup.lock`. Ordinary exclusive opens do not capture
  a pre-shutdown redo prefix and do not restore one after
  `mysql_server_end()`.
- Ordinary exclusive opens still hold `mylite.lock`, so there is no peer
  process to coordinate with while the embedded runtime is shutting down.
- `packages/libmylite/tests/embedded_open_close_test.c` has
  `test_open_close_repeatedly()`, but it opens only twice and does not create
  or reopen an InnoDB table. That misses the performance-probe shape: create an
  InnoDB table, close, then perform several full embedded warm reopen cycles.
- Local production reproduction on 2026-06-09:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1` reproduced the checksum abort during the
  ordinary warm-open loop. One-, two-, three-, and a later five-iteration
  minimal run passed, so the failure is intermittent.

## Design

Generalize the existing in-memory redo startup-prefix snapshot to ordinary
exclusive shutdown:

- On final runtime release for any non-read-only, non-memory database, capture
  the current redo startup prefix before `mysql_server_end()` if it validates
  as a MariaDB-current checkpoint prefix.
- For ownerless shutdown, keep the existing rule: restore the prefix only when
  the final ownerless runtime holds the startup lock and no live peers remain.
- For ordinary exclusive shutdown, restore the captured prefix after
  `mysql_server_end()` because `mylite.lock` excludes other processes and the
  runtime is the final local owner.
- Before writing the captured prefix, re-read the post-shutdown redo startup
  prefix and skip the write when MariaDB already left a valid current
  checkpoint prefix. This keeps normal close timing from adding a write/fsync
  tax in the common path.
- Do not introduce a new durable file for ordinary mode. The snapshot is
  process-local and only covers the current close path.
- Do not relax MariaDB redo validation. If a prefix cannot be captured or the
  restore write fails, keep the existing close behavior and surface failures
  through focused tests/probe evidence rather than accepting a corrupt header.

This is a bounded repair for embedded repeated startup/shutdown. It is not a
replacement for broader native redo/checkpoint reconciliation.

## Compatibility Impact

No SQL, C API, PHP API, DDL, or wire-protocol behavior change is intended.
The observable change is that repeated ordinary open/close cycles over a
directory containing InnoDB state should remain restartable instead of
intermittently failing InnoDB startup.

## Database-Directory And Native Storage Impact

No new directory entry or durable format is added. When repair is needed, it
writes the existing native InnoDB redo startup prefix in `datadir/ib_logfile0`
after embedded shutdown, using a prefix that already passed MariaDB-current
redo checkpoint validation before shutdown. When the post-shutdown prefix is
already valid, the close path only performs the validation read and does not
rewrite the redo file.

## Binary Size And Dependency Impact

No new dependency. The implementation reuses existing first-party redo-prefix
helpers and embedded open/close perf counters.

## Test Plan

- Add focused baseline coverage that creates an InnoDB table and performs at
  least five full ordinary close/reopen cycles, verifying rows after each
  reopen.
- Run the focused baseline open/close test under `php-embedded-prod`.
- Run the minimal production performance-probe reproduction with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1`.
- Run the default stats-off production performance probe.
- Run the reduced stats-enabled ownerless attribution probe to ensure the
  previous undo-cache evidence remains unchanged.
- Run production build-type guards, format check, and whitespace check.

## Acceptance Criteria

- Ordinary repeated warm reopen no longer reproduces the redo log-header abort
  in the focused test or minimal production probe.
- Ownerless no-live shutdown redo-prefix repair behavior is preserved.
- Active-runtime reconnect timing remains measured separately from full
  `mysql_server_end()` startup/shutdown cost.
- CI timing jobs continue to use production builds and fail early on stale
  non-production build directories.

## Verification

Local verification on 2026-06-09 used production embedded builds:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_open_close_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_embedded_open_close_test
  innodb-open-close-repeatedly` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-open-close$' --output-on-failure` passed in `11.14s`.
- The minimal production repeated-open probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1`. It reported ordinary warm open/close
  `405.812ms`, ordinary open `162.192ms`, ordinary close `243.617ms`,
  ordinary redo capture `0.044ms`, ordinary conditional redo restore
  `0.060ms`, ownerless warm open/close `391.167ms`, ordinary active-runtime
  reconnect `1.245ms`, and ownerless active-runtime reconnect `1.031ms`.
- The default stats-off production probe passed. It reported ordinary warm
  open/close `455.036ms`, ownerless warm open/close `416.139ms`, ordinary
  active-runtime reconnect `3.105ms`, ownerless active-runtime reconnect
  `0.771ms`, ordinary `mysql_server_init` `135.863ms`, ordinary
  `mysql_server_end` `311.786ms`, ownerless `mysql_server_init` `131.261ms`,
  ownerless `mysql_server_end` `279.300ms`, ownerless transaction insert ratio
  `0.7922`, and ownerless autocommit insert ratio `0.2143`.
- The reduced stats-enabled ownerless attribution probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. The previous single-owner
  undo-cache evidence stayed intact: `100` reuse attempts, `81` hits, `19`
  misses, zero ownerless cache-reuse skips, zero blocked ownerless history
  cache cases, and `0.0000` blocked-ownerless ratio.
- Focused production ownerless SQL selectors passed: `commit-race`,
  `single-owner-skip-peer-history`, and `live-reclaim`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and hook selectors
  `redo-header-backup-validation`, `visible-publish-crash`, and
  `visible-checkpoint-crash` passed.
- Production build guards passed:
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`, and
  `tools/require-cmake-release-build build/ownerless-test-hooks`.
- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed, and a workflow grep
  audit found no CI uses of `dev`, `embedded-dev`, or `php-embedded-dev`
  presets/build directories.
- `cmake --build --preset format-check-prod` passed.

## Risks And Follow-Up

- Restoring an older valid checkpoint prefix after shutdown is conservative for
  restartability but can increase recovery work if MariaDB advanced the
  checkpoint during shutdown. The implementation now skips the write when the
  post-shutdown prefix validates, and the repeated-open test verifies user rows
  survive repeated reopen.
- This slice does not solve broader ownerless native redo/checkpoint
  reconciliation or long-running multi-process DDL/file lifecycle recovery.
