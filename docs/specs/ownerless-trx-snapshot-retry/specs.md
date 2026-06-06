# Ownerless Transaction Snapshot Retry

## Problem

Ownerless stress once aborted in `trx_sys_t::snapshot_ids()` after
`mylite_ownerless_trx_snapshot()` returned a non-OK/non-FULL ownerless result.
The isolated BLOB pressure stress and full stress rerun passed, so the observed
failure is not tied to the generated-column matrix slice. It still exposes a
hardening gap: a transient ownerless transaction-registry snapshot miss becomes
an unexplained InnoDB `ut_error` instead of retrying the registry snapshot path
that already uses a bounded latch wait.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0sys.h` builds MVCC read-view
  transaction snapshots in `trx_sys_t::get_max_trx_id()` and
  `trx_sys_t::snapshot_ids()`. MyLite ownerless hooks replace the process-local
  transaction hash with the directory-owned transaction registry when hooks are
  enabled.
- `mariadb/storage/innobase/trx/mylite_ownerless_trx_hooks.cc` forwards
  `mylite_ownerless_trx_snapshot()` to the current MyLite callback and returns
  `MYLITE_OWNERLESS_TRX_UNAVAILABLE` only when no snapshot hook is installed.
- `packages/libmylite/src/database.cc` maps
  `mylite_ownerless_trx_registry_snapshot_read_view()` results to the InnoDB
  hook result. Registry `OK` and `FULL` are expected; other registry results are
  currently mapped to `MYLITE_OWNERLESS_TRX_ERROR`.
- `packages/libmylite/src/ownerless_trx_registry.cc` snapshot functions acquire
  the registry latch with a bounded wait and can return a non-OK result for a
  transient timeout or inconsistent snapshot observation.

## Design

Add a MyLite-owned retry helper beside the transaction hook bridge:
`mylite_ownerless_trx_snapshot_retry()`. It calls
`mylite_ownerless_trx_snapshot()` and retries a small bounded number of times
when the hook returns `MYLITE_OWNERLESS_TRX_ERROR`. It immediately returns
`OK`, `FULL`, or `UNAVAILABLE`, preserving the existing fallback and fatal
semantics for persistent errors.

Use the retry helper only for snapshot reads in `trx_sys_t::get_max_trx_id()`
and `trx_sys_t::snapshot_ids()`. Do not retry transaction ID allocation,
registration, transaction-number assignment, or deregistration in this slice.

Extend `mylite_embedded_ownerless_trx_hooks_test` so its snapshot hook injects
a single transient `MYLITE_OWNERLESS_TRX_ERROR` before a read-view statement.
Without the retry helper, the test would enter the existing InnoDB fatal path.

## Scope And Non-Goals

In scope:

- Bounded retry for transient ownerless transaction snapshot hook errors.
- Deterministic embedded hook coverage for one transient snapshot error.
- Documentation of the retry boundary and stress evidence.

Out of scope:

- Redesigning the transaction registry latch or shared-memory layout.
- Retrying persistent registry corruption indefinitely.
- Changing SQL-visible transaction isolation semantics.
- Changing ownerless allocation, registration, assignment, or deregistration
  hook behavior.

## Compatibility Impact

No SQL semantics change. This preserves MariaDB MVCC behavior while making the
ownerless transaction snapshot bridge more tolerant of transient registry
snapshot failures under stress.

## Directory And Lifecycle Impact

No directory layout changes. The helper only changes how a process reads the
existing ownerless shared transaction registry through installed hooks.

## Native Storage Impact

No native storage format changes. The change affects InnoDB read-view snapshot
construction only when ownerless transaction hooks are enabled.

## Public API Impact

No public `libmylite` API change. The new helper is an internal MariaDB hook
bridge function.

## Binary Size Impact

Negligible production binary-size impact: one small retry helper and one test
injection path.

## Test Plan

- Build `mylite_embedded_ownerless_trx_hooks_test` in `embedded-dev`.
- Run `mylite_embedded_ownerless_trx_hooks_test`.
- Run ownerless primitive transaction-registry coverage.
- Run focused BLOB pressure stress and the full `ownerless-stress` preset.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- A single injected ownerless transaction snapshot error is retried and the
  InnoDB SQL statement succeeds.
- Normal ownerless transaction hook coverage still observes full snapshot
  retries, register/deregister balance, and zero leaked active transactions.
- Focused ownerless stress pressure cases pass.
- Persistent non-snapshot hook behavior remains unchanged.

## Risks And Follow-Up

- This does not prove the root cause of a rare registry snapshot timeout; it
  keeps a transient miss from turning into an immediate fatal assertion.
- If repeated stress runs still produce the same assertion after this change,
  the next slice should instrument the registry latch owner and snapshot
  mismatch reason directly.
