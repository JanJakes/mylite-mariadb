# Ownerless Startup Retry Budget

## Problem

Ownerless startup already serializes native embedded startup and retries a
bounded number of `mysql_server_init()` failures after cleanup and redo-prefix
restore. Concurrent DDL stress still exposed an intermittent opener abort where
InnoDB reported `DB_CORRUPTION` as `Data structure corruption` and the embedded
server surfaced `Unknown/unsupported storage engine: InnoDB` before the stress
harness could retry the SQL operation. A clean rerun passed, which points at a
transient native startup/recovery window rather than a deterministic SQL
semantic failure.

The current three-attempt budget is too small to prove that the documented
failure-then-restore retry path can absorb more than a couple of transient
native startup failures.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/ut/ut0ut.cc` maps `DB_CORRUPTION` to
  `Data structure corruption`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` `innodb_init()` calls
  `srv_start()` during plugin initialization and returns through
  `innodb_init_abort()` when native startup fails.
- `packages/libmylite/src/database.cc` `open_impl()` already holds
  `concurrency/mylite-runtime-startup.lock` across ownerless runtime startup,
  connection, system table bootstrap, and dictionary generation.
- `packages/libmylite/src/database.cc` `start_runtime()` already calls
  `mysql_server_end()`, restores a captured or saved 12 KiB redo startup prefix,
  unmaps ownerless runtime state, and clears process-global runtime state after
  `mysql_server_init()` failure.
- `docs/specs/ownerless-runtime-startup-serialization/specs.md` documents
  bounded startup retry after partial MariaDB embedded startup cleanup and
  redo-prefix restore as the intended ownerless/native recovery behavior.

## Design

Raise the ownerless/native startup failure budget from three attempts to a
small larger bounded value. Successful startup still calls `start_runtime()`
once; the extra attempts exist only after `start_runtime()` returns
`MYLITE_ERROR`.

Add an unsafe-hook-only deterministic fault at the strongest local seam:
immediately after `mysql_server_init()` succeeds and before MyLite marks the
runtime initialized. The fault consumes a process-local counter from the test
environment, converts that startup into the same failure path used by real
MariaDB startup errors, and therefore exercises:

- `mysql_server_end()` after partial embedded initialization;
- redo startup-prefix restore;
- ownerless shared-memory and runtime-state cleanup;
- the retry loop in `open_impl()`;
- final successful ownerless/native reopen after several forced failures.

The fault is not compiled into production builds and is not a SQL feature.

## Compatibility Impact

No public C API, SQL syntax, directory layout, native file format, or ordinary
successful-open behavior changes. Failed native startup can take slightly
longer before returning a hard error because MyLite gives transient native
startup/recovery windows more bounded chances to settle.

## Directory And Native Storage Impact

No new durable files or formats. The existing redo startup-prefix capture and
restore path remains the only native-file mutation in the retry failure path.

## Test Plan

- Add hook-build selector `runtime-startup-retry-budget`.
- The selector initializes an ownerless database, forces four
  post-`mysql_server_init()` failures, and verifies the ownerless open still
  succeeds and can update/read the application table.
- The selector then forces four ordinary native read/write startup failures
  after ownerless activity and verifies native reopen succeeds and sees the
  committed state.
- Run the focused selector in `ownerless-test-hooks`.
- Run adjacent startup/redo selectors:
  `redo-header-backup-validation`, `commit-race`, and `native-reclaim`.
- Run the ownerless DDL stress subset because the observed failure came from
  that path.
- Run production-build guards and formatting checks.

## Acceptance Criteria

- Successful startup remains single-attempt on the common path.
- Hook-only forced failures after successful `mysql_server_init()` use the same
  cleanup/restore path as real startup failures.
- Four forced failures succeed within the new bounded retry budget for both
  ownerless and ordinary native reopen after ownerless activity.
- The docs keep broader native redo/checkpoint reconciliation and DDL/file
  lifecycle recovery as remaining completion gates.

## Risks And Follow-Up

This slice reduces a real transient startup/recovery exposure but does not
prove arbitrary redo/checkpoint reconciliation, does not change InnoDB recovery
semantics, and does not replace the broader native redo/checkpoint matrix. If
DDL stress still produces startup corruption after the larger bounded budget,
the next slice should reduce that native recovery window directly rather than
raising the budget again.
