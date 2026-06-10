# Ownerless Monotonic Page-Version Visibility

## Problem Statement

Production `ownerless-stress` random transaction runs exposed a reader
regression while peer workers committed only positive deltas:

```text
ownerless random tx stress reader mismatch: sum=<new> previous_sum=<old>
```

The final ownerless/native reopen oracle still checks exact totals, but the
live reader failure showed a more immediate problem: one ownerless handle could
observe a newer page-version state and later see an older or speculative page
image. That made PHPUnit and stress timing noisy because correctness failures
looked like unstable performance.

## Source Findings

- `trx_t::commit_in_memory()` deregisters the InnoDB read-write transaction
  before the ownerless page-visible publish path, so the shared transaction
  registry can gate peer transactions without blocking the committing
  transaction's own visibility publication.
- `trx_t::rollback_low()` previously published transaction pages after
  `rollback_finish()` with `log_get_lsn()`, after the deferred ownerless page
  list and transaction-local context could no longer prove a committed
  boundary.
- `mtr_t::ownerless_page_write_publish_boundary()` could publish the current
  dirty data-page image while an explicit transaction still held uncommitted
  row versions on that page.
- `buf_flush_publish_ownerless_pages_to_lsn()` scans a process-local buffer
  pool and cannot prove row-level commit ownership for arbitrary dirty
  data/index pages.
- `refresh_ownerless_external_pages_before_statement()` could promote
  autocommit read statements to the live raw latest LSN during an idle window
  and later fall back to the older page-visible LSN when another writer
  started.
- `ownerless_innodb_page_read_locked()` could return an index-miss
  `UNAVAILABLE` before taking a stable WAL snapshot, so a later WAL append
  could be missed by a page-index-generation negative cache.

## Design

The slice keeps ownerless page-version reads conservative until a boundary is
proven:

- Each ownerless handle records a monotonic page-version read LSN for eligible
  direct or prepared `SELECT`/`WITH` statements.
- The handle publishes a shared page-version pin for the chosen read LSN before
  clean buffer-pool refresh. Older clean local pages can then be evicted only
  while WAL retention is protected. Successful direct `mylite_exec()` reads keep
  that handle pin until a replacement read, non-read/current-read statement,
  error path, or close, matching the prepared-statement retention model closely
  enough for live-peer reclamation to see stale process-local pages between
  statements.
- No-live reclaim does not let a runtime that only consumed the current visible
  page-version WAL truncate that WAL on final close. Writer runtimes keep the
  normal statement/timer reclaim path, no-live writer close still needs native
  page proof before truncating retained WAL, and older pinned-snapshot readers
  can release their pins and allow post-release checkpointing.
- Ordinary exclusive read/write reopen with retained ownerless page-version WAL
  or a nonzero ownerless checkpoint-visible boundary force-refreshes clean
  process-local InnoDB buffer-pool pages from that boundary before SQL runs, so
  same-process embedded restart cannot reuse stale clean pages left by an
  earlier non-ownerless open after peer-close checkpointing has compacted the
  page-version WAL.
- Ownerless handles also bypass the single-owner clean-page refresh skip when
  they first observe a new shared process generation or advance an older
  retained handle pin to a newer page-version read LSN. The ordinary
  steady-state single-owner skip remains available after that mandatory
  refresh boundary has been crossed.
- Live-peer reclaim and non-DDL no-live DML reclaim scan checkpointable user
  tablespace page-version records before truncating the WAL and require the
  native tablespace page on disk to have either a `FIL_PAGE_LSN` newer than the
  record page LSN, or the same `FIL_PAGE_LSN` plus a byte-for-byte match with
  the retained payload. If that page-level proof is unavailable, reclaim keeps
  the WAL even when the global native checkpoint LSN covers the visible commit
  boundary.
- Live-peer and no-live reclaim do not run from a writer runtime that has local
  writes but has not consumed the current visible page-version WAL after those
  writes. That keeps immediate writer-close cleanup from racing a peer's native
  page refresh while still allowing read-after-write peer reclaim to use the
  normal proof path.
- Ownerless `COMMIT` and full `ROLLBACK` ending an explicit transaction with
  local writes take the global ownerless write statement lock and refresh
  current shared native state before executing, so concurrent explicit commits
  cannot overwrite shared InnoDB support-page visibility from independent
  process-local images. Read-only transactions that only used native locking
  reads keep conservative active-transaction refresh behavior, but their
  transaction-end SQL is not queued behind a peer writer's global ownerless
  statement gate.
- Autocommit reads promote from durable page-visible LSN to live raw latest LSN
  only when no explicit ownerless transaction, no shared read-write
  transaction, and no active redo reservation is present.
- Eligible autocommit page-version reads close the current InnoDB read view at
  statement start so a later statement can observe a peer commit instead of
  reusing an older ownerless snapshot.
- Explicit repeatable-read and serializable transactions continue to pin their
  first consistent-read LSN; `START TRANSACTION WITH CONSISTENT SNAPSHOT` uses
  the same handle-observed lower bound.
- Explicit read-committed transactions do not pin a repeatable snapshot. Each
  eligible read can advance to the live read LSN when the transaction has not
  performed local writes or locking reads and no other live explicit
  transaction, shared read-write transaction, or redo reservation is active.
- Page-visible publication is skipped while another live process slot is
  inside an explicit transaction or the shared transaction registry still has
  an active read-write transaction owned by another process.
- Commit visibility uses the transaction-local commit boundary. Rollback no
  longer promotes visibility with a post-cleanup global `log_get_lsn()`.
- If a transaction-owned page image has a `FIL_PAGE_LSN` newer than the commit
  boundary, that page gets an individual page-version record at the observed
  page LSN; the global page-visible LSN is not promoted to that page LSN.
- The slow commit path flushes native dirty pages through the current InnoDB
  log LSN before publishing that higher visible boundary. That keeps later live
  readers from pinning a raw latest LSN whose page image is neither in the
  ownerless WAL nor durable on disk.
- Transaction-deferred data/index pages are published at commit/rollback
  visibility boundaries; mini-transaction dirty-current-page publication no
  longer creates boundary records for uncommitted user pages.
- Before a new explicit transaction writes a data/index page, a dirty
  process-local page left by earlier work is force-refreshed under the
  ownerless page-write lock unless that same transaction has already modified
  the page or the single-owner proof can show that no external peer image can
  exist. The forced refresh can overlay a visible native disk page even when
  the local page LSN is not lower, preventing a stale full-page image from
  erasing peer commits on the same physical page.
- SQL `SELECT`, including locking reads such as `SELECT ... FOR UPDATE`, does
  not take ownerless page-write ownership; row-lock and current-read waits stay
  on the native InnoDB paths instead of being hidden behind page-write retries.
- The generic InnoDB page read-complete overlay is limited to MyLite-classified
  plain `SELECT`/`WITH` page-version reads. Non-SELECT DDL and DML use the
  explicit page-write refresh and publication paths instead, so retained
  ownerless page-version images cannot be copied into native DDL rebuild reads.
- The per-space transaction page-write gate remains statement-scoped: explicit
  transactions release gate markers at statement end so unrelated writers in
  the same tablespace are not serialized for the transaction lifetime. Real
  dirty page-write ownership remains transaction-scoped until commit or
  rollback after page-level ownerless acquisition records the modified page,
  keeping the conservative serialization proof alive when later callbacks miss
  a user page.
- The global dirty-page publish scan is limited to native support/allocation
  and system pages. User data/index pages require transaction-owned publish or
  native snapshot-boundary synthesis.
- Page-index misses take an authoritative WAL snapshot and scan before a
  negative result can be cached. Direct page-index hits still validate the WAL
  tail before returning.

The page-version latest-visible rule remains commit-LSN first with page LSN as
the tiebreaker. Page LSN is native freshness evidence, not a replacement for
the visible commit boundary.

## Affected Subsystems

- MyLite ownerless statement refresh and page-version pinning.
- InnoDB ownerless page-visible publication and rollback handling.
- InnoDB dirty-page/page-write publication boundaries.
- Ownerless page-log negative-cache behavior.
- Ownerless random transaction stress reader and failure oracles.

No public C API, PHP API, wire protocol, native page format, shared-memory
layout, or page-version WAL record format changes.

## Compatibility And Performance Impact

The slice improves ownerless read correctness for one handle: once a handle has
observed an eligible page-version read LSN, later eligible reads on that handle
do not move backward. It also avoids publishing page-visible state while peer
explicit transactions can still hold lower-LSN or uncommitted page images.

The tradeoff is deliberate: under continuous explicit writers, live-read
promotion may wait for durable page-visible progress instead of sampling raw
latest LSN. This can reduce speculative ownerless read throughput, but it keeps
production timing evidence tied to correct visible states. Direct read pins can
now persist across autocommit statement boundaries, so live peers that have
actually read page-version state retain WAL until they close, replace, or error
out of the pin. Statement and timer checkpoint scheduling ignore pins owned by
the current single owner while still blocking peer-owned pins, preserving the
single-owner reclaim/refresh fast paths. A pure reader that consumes the
current visible page-version WAL may retain it for the next no-live recovery
instead of truncating it on close, which favors correctness over a misleading
checkpoint timing win. The slow-path native flush can add work for explicit
write commits, but it is bounded to the slow path and prevents false production
timing wins from unproven visibility.
The same single-owner proof skips explicit-transaction buffer-pool first-write
refresh, so a single-owner prepared insert transaction does not scan retained
ownerless WAL for every statement. A reduced production attribution probe
reported ownerless explicit-transaction inserts at 1340 ops/s versus ordinary
inserts at 2219 ops/s, with zero ownerless page-read or page-log scan calls,
after the skip was applied.
The current reduced stats-enabled autocommit probe also reports zero
non-SELECT ownerless page-read probes after read-complete overlay gating, with
remaining ownerless cost concentrated in page-version append and commit-MTR
publication. That keeps the monotonic read overlay attached to the statements
that need it instead of charging DDL/DML rebuild paths.
Ordinary non-ownerless opens and fresh WordPress CI build steps remain on
production Release or MinSizeRel build modes and are not changed by this slice.

This does not claim full external MariaDB/RQG stress completion, broader
native redo/checkpoint reconciliation, full DDL/file lifecycle recovery, or
SQL-level table-lock fault-injection coverage. Prior SQL table-lock shapes
still stopped before the ownerless table-wait callback.

## Test And Verification Plan

- Build the MariaDB embedded archive with the production `MinSizeRel` baseline.
- Build ownerless stress targets with production MyLite artifacts.
- Run repeated production `random-tx-stress` loops at 24 rounds.
- Run focused CTest ownerless page-version, primitive, hook, and random
  transaction selectors affected by the visibility fences.
- Run adjacent ownerless stress cases that use explicit transactions,
  savepoints, active readers, and page-version reclamation.
- Run production build guards for MyLite and MariaDB embedded caches.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- A single ownerless handle never lowers the page-version read LSN used for
  eligible plain `SELECT`/`WITH` statements.
- Random transaction stress readers no longer observe decreasing positive row
  aggregates under repeated production stress runs.
- Final ownerless/native reopen oracles for random transaction stress continue
  to match exact expected totals before and after forced `.shm` rebuild.
- Page-visible publication is not based on uncommitted dirty data pages,
  post-rollback global LSNs, or global dirty user-page scans.
- Live-peer page-log reclaim and non-DDL no-live DML reclaim do not discard a
  checkpointable user tablespace page-version record while the native data-file
  page still has an older `FIL_PAGE_LSN`, or the same `FIL_PAGE_LSN` with
  different page bytes.
- CI timing-bearing jobs continue to use production MyLite builds and
  `MinSizeRel` MariaDB embedded archives, with PHPUnit test-only phases
  separated from build phases for visible timings.

## Remaining Risks

- Live-read promotion is conservative while explicit peer transactions are
  active; follow-up performance work should measure its impact under sustained
  explicit-writer load.
- Broader DDL/dictionary/space-allocation classes, full native redo/checkpoint
  reconciliation, external MariaDB/RQG stress, and broader active-reader
  pressure policy remain planned ownerless gaps.
