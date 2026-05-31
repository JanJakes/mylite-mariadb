# Ownerless Active-Reader Pressure Limit

## Problem

Ownerless repeatable-read and serializable readers can pin page-version
visibility while peer writers keep committing. Existing active-reader pressure
coverage proves bounded writer progress and close-time reclamation after the
reader releases, but it deliberately leaves user-visible pressure policy
unimplemented. Applications that prefer bounded retained WAL over writer
progress need a deterministic way to stop new ownerless writes once active
snapshot pins have already forced the page-version WAL past a configured byte
limit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` initializes transactions at
  repeatable-read isolation by default and, on the ownerless commit path, calls
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` before
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()`.
- `mariadb/storage/innobase/read/read0read.cc` publishes InnoDB read views
  through `ReadView::publish_ownerless()` and removes them from
  `ReadView::close()`. MyLite separately publishes page-version read LSN pins
  for ownerless repeatable-read and serializable transactions in
  `packages/libmylite/src/database.cc`.
- `mariadb/storage/innobase/buf/buf0flu.cc` publishes dirty page images to
  MyLite with `mylite_ownerless_innodb_publish_page_version()` before MyLite
  exposes the page-visible LSN to peers.
- `packages/libmylite/src/database.cc` stores shared page-version pins in the
  page-pin registry, uses that registry in
  `reclaim_ownerless_page_log_after_native_checkpoint()`, and can reclaim with
  live peers only when no pins exist or active-pin boundary proof is complete.
- `packages/libmylite/src/ownerless_page_log.cc` treats the durable
  `concurrency/mylite-concurrency.wal` prefix as the source of page-version
  records that active readers may need until checkpoint reclamation proves the
  records can be compacted.

## Design

Add an opt-in `mylite_open_config.ownerless_page_log_limit_bytes` field.

Semantics:

- `0` means unlimited and preserves current behavior.
- The field applies only to `MYLITE_OPEN_OWNERLESS_RW` handles.
- Before a direct or prepared SQL statement that MyLite classifies as a write
  enters MariaDB execution, MyLite snapshots the shared page-version pin
  registry and stats `concurrency/mylite-concurrency.wal`.
- If at least one page-version snapshot pin is active, the WAL contains retained
  page-version records beyond the empty header, and the WAL size is greater
  than or equal to the configured limit, MyLite returns `MYLITE_BUSY` with a
  MyLite-owned diagnostic before dispatching the write.
- Read statements, transaction control, `COMMIT`, `ROLLBACK`, and write
  statements after the active pins release are not blocked by this policy.

The limit is intentionally a pre-dispatch soft cap. A write that starts while
the retained WAL is below the limit may grow the WAL past the limit; the next
configured write observes the pressure and returns busy until reclamation or
reader release reduces the retained WAL.

## Scope And Non-Goals

In scope:

- Public C API configuration for an ownerless page-version WAL pressure limit.
- Direct `mylite_exec()` write dispatch enforcement.
- Prepared-statement `mylite_step()` write dispatch enforcement.
- Focused SQL coverage proving direct and prepared writes return `MYLITE_BUSY`
  while a reader pins retained WAL, and prepared retry succeeds after the
  reader releases.

Out of scope:

- Background checkpoint workers or asynchronous reclamation.
- A hard post-commit byte guarantee.
- WAL-size diagnostics APIs, callback hooks, or automatic reader cancellation.
- External MariaDB/RQG long-running oracle stress.
- Changing MariaDB isolation semantics or native storage formats.

## Compatibility Impact

Default behavior is unchanged because the limit defaults to `0`. When an
application opts in, ownerless write statements can return `MYLITE_BUSY` before
MariaDB execution under active-reader page-version pressure. That is an
embedded MyLite resource policy, not a MariaDB SQL semantics change. MariaDB
errno remains zero for this MyLite-owned policy failure.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The policy reads existing
directory-owned ownerless state:

- `concurrency/mylite-concurrency.shm` for page-version snapshot pins,
- `concurrency/mylite-concurrency.wal` for retained page-version WAL bytes.

Close-time reclamation and forced `.shm` rebuild behavior are unchanged.

## Native Storage Impact

No native InnoDB format changes. The policy runs before SQL write dispatch and
does not alter page-version append, flush, checkpoint, or native recovery
rules. Once active readers release and the existing checkpoint path reclaims the
WAL, the same configured writer can proceed.

## Public API Impact

`mylite_open_config` gains a trailing
`ownerless_page_log_limit_bytes` field. The struct is size-gated, so older
callers that pass a smaller `size` retain current unlimited behavior.

## Binary Size Impact

The implementation adds a small first-party policy check and one focused SQL
test. It adds no dependencies and does not change the embedded MariaDB profile.

## Test Plan

- Add a focused `active-reader-pressure-limit` ownerless SQL selector.
- The selector holds a repeatable-read snapshot in a child, commits one peer
  write to make retained WAL non-empty, opens another ownerless writer with the
  limit set to the retained WAL size, and verifies:
  - direct `UPDATE` returns `MYLITE_BUSY`,
  - prepared `UPDATE` returns `MYLITE_BUSY` at `mylite_step()`,
  - no blocked write is applied,
  - the same prepared statement succeeds after the reader releases and
    close-time reclamation runs, and
  - final ownerless/native reopen after forced `.shm` rebuild preserves data
    and checkpoints the WAL.
- Run focused embedded and hook selectors, full embedded and hook ownerless SQL,
  ownerless stress, `format-check`, and diff checks.

## Acceptance Criteria

- The default ownerless path remains unlimited.
- Configured ownerless direct and prepared writes return `MYLITE_BUSY` while
  active pins retain WAL at or above the configured limit.
- Policy failures happen before MariaDB execution and do not change row data or
  set MariaDB errno.
- Releasing the reader and reclaiming the WAL allows a retry to succeed.
- Documentation and compatibility matrices describe the limit and remaining
  broader pressure-policy gaps accurately.

## Risks And Follow-Up

- The limit is a soft pre-dispatch cap, not a strict maximum retained byte
  count.
- Applications still need their own retry policy for `MYLITE_BUSY`.
- Ownerless pressure diagnostics are covered separately by
  `ownerless-pressure-diagnostics`; broader follow-up remains for background
  checkpoint pressure, automatic checkpoint scheduling, and external
  long-running MariaDB/RQG stress.
