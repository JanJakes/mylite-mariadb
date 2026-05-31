# Ownerless Pressure Diagnostics

## Problem

Ownerless page-version readers can pin an older read LSN while peer writers
continue to append dirty page images to
`concurrency/mylite-concurrency.wal`. MyLite now has an opt-in write throttle
for this condition, but applications cannot inspect the pressure state that
will make a configured writer return `MYLITE_BUSY`. Operators also have no
public signal that distinguishes "no active pinned readers" from "a live
snapshot is retaining page-version WAL".

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc` handles
  `START TRANSACTION WITH CONSISTENT SNAPSHOT` by setting transaction state
  before calling `ha_start_consistent_snapshot()`.
- `mariadb/sql/handler.cc` dispatches consistent snapshot creation to each
  participating engine while holding `LOCK_commit_ordered`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements InnoDB consistent
  snapshot startup by starting the transaction and opening `trx->read_view`
  except at `READ UNCOMMITTED`.
- `mariadb/storage/innobase/read/read0read.cc` opens and closes InnoDB
  `ReadView` instances and the MyLite fork publishes ownerless read-view state
  from `ReadView::publish_ownerless()` and removes it from
  `ReadView::unpublish_ownerless()`.
- `packages/libmylite/src/database.cc` separately publishes page-version read
  LSN pins for ownerless repeatable-read and serializable transactions through
  `ensure_ownerless_transaction_page_version_pin()` and releases them at
  transaction end.
- `packages/libmylite/src/ownerless_page_pin_registry.cc` provides
  `mylite_ownerless_page_pin_registry_snapshot_oldest()`, which returns the
  active pin count and the oldest pinned read LSN under the registry latch.
- `packages/libmylite/src/database.cc` already uses that pin snapshot and
  `fstat()` of `concurrency/mylite-concurrency.wal` to enforce
  `mylite_open_config.ownerless_page_log_limit_bytes` before direct or
  prepared write dispatch.

## Design

Add a public `mylite_ownerless_pressure_status()` C API that reports a
size-gated `mylite_ownerless_pressure_info` struct.

The first struct version reports:

- active page-version snapshot pin count,
- oldest active page-version pin LSN, or zero when no pins are active,
- current raw `concurrency/mylite-concurrency.wal` file size in bytes,
- the handle's configured `ownerless_page_log_limit_bytes`, and
- whether the current state matches the existing configured write throttle:
  active pin, non-empty page-version WAL payload, non-zero limit, and WAL size
  at or above the limit.

The implementation factors the existing pressure-limit state read into a shared
helper so the public diagnostic and pre-write throttle use the same pin and WAL
facts. The API is read-only: it does not trigger checkpointing, cleanup stale
owners, cancel readers, or change MariaDB state.

Non-ownerless handles return `MYLITE_OK` with zeroed fields because no
ownerless write-pressure policy applies to that handle. Ownerless handles
return `MYLITE_IOERR` if the required shared-memory or WAL state is unavailable
or unreadable. Invalid arguments return `MYLITE_MISUSE`.

## Scope And Non-Goals

In scope:

- Public C API struct and status function.
- Shared pressure-state helper for the API and existing write throttle.
- API validation coverage for null pointers and undersized output structs.
- Focused ownerless SQL coverage proving active pin count, oldest pin LSN, WAL
  byte reporting, configured limit reporting, and limit-reached reporting.
- Documentation and compatibility matrix updates.

Out of scope:

- Background checkpoint scheduling or pressure workers.
- A hard post-commit WAL byte limit.
- Reader cancellation, callbacks, or wait-notification APIs.
- New directory files or native InnoDB format changes.
- External MariaDB/RQG long-running stress.

## Compatibility Impact

Default SQL behavior is unchanged. The new function is a MyLite C API
extension for embedded ownerless diagnostics; it does not change MariaDB SQL
semantics. Existing callers remain ABI-compatible because the new output struct
is size-gated and no existing public struct field is reordered.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The diagnostic reads existing
ownerless state in the database directory:

- `concurrency/mylite-concurrency.shm` for page-version snapshot pins,
- `concurrency/mylite-concurrency.wal` for the current retained page-version
  WAL file size.

The API does not persist new state and does not alter open, close, recovery,
checkpoint, or forced `.shm` rebuild behavior.

## Native Storage Impact

No native InnoDB format or page-version record encoding changes are introduced.
The diagnostic observes page-version pressure after MariaDB/InnoDB and MyLite
hooks publish the same state already used by ownerless reads and the configured
write throttle.

## Public API Impact

`mylite/mylite.h` gains:

- `mylite_ownerless_pressure_info`, a growable output struct with a `size`
  field,
- `mylite_ownerless_pressure_status()`, returning stable MyLite result codes.

Callers zero-initialize the struct, set `size = sizeof(info)`, and pass a live
`mylite_db *` handle.

## Binary Size Impact

The slice adds a small first-party status helper and tests. It adds no
dependencies and does not change the embedded MariaDB profile.

## Test Plan

- Extend `mylite_api_test` with validation for null handles, null output, and
  undersized output structs.
- Add an `active-reader-pressure-diagnostics` ownerless SQL selector that:
  - starts from a checkpointed ownerless database,
  - verifies zero pressure with no active pin,
  - holds a repeatable-read snapshot pin in a child process,
  - commits a peer update that leaves retained page-version WAL,
  - opens a configured ownerless handle at the retained WAL size and verifies
    active pin count, oldest pin LSN, WAL bytes, configured limit, and
    limit-reached state,
  - releases the reader, verifies pressure clears and a configured write can
    proceed, and
  - verifies final ownerless and native reopen after forced `.shm` rebuild.
- Run focused embedded and hook selectors, API tests, full embedded and hook
  ownerless SQL, ownerless stress, `format-check`, and diff checks.

## Acceptance Criteria

- Public diagnostics report the same active-pin/WAL facts that drive the
  existing write throttle.
- Invalid API usage returns `MYLITE_MISUSE`.
- Non-ownerless handles report no ownerless pressure.
- Ownerless diagnostics expose active-reader pressure while a snapshot pin is
  live and report no active pressure after the reader releases and reclamation
  runs.
- Documentation explicitly keeps background checkpoint scheduling and external
  long-running stress as follow-up work.

## Risks And Follow-Up

- The reported WAL byte value is the raw ownerless WAL file size, including
  fixed recovery and page-log headers, because that is the value enforced by
  the existing soft cap.
- The diagnostic is instantaneous; a peer can change pin or WAL state
  immediately after the call returns.
- Follow-up remains for background checkpoint scheduling, broader active-reader
  pressure policy, and external MariaDB/RQG oracle stress.
