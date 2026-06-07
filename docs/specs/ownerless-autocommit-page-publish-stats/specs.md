# Ownerless Autocommit Page Publish Stats

## Problem

Ownerless autocommit writes remain much slower than ordinary embedded
autocommit writes after the WordPress PHPUnit phase split and smaller
hot-path optimizations. Current focused evidence shows:

- ordinary WordPress mysqli/PHPUnit work is close to trunk when compared with
  matching source and storage placement,
- ownerless transactional inserts are much faster than ownerless autocommit
  inserts because they amortize commit publication work,
- ownerless autocommit insert throughput stays low across `FULL`, `NORMAL`,
  and `OFF` durability, pointing at the ownerless visibility bridge rather than
  MariaDB redo policy alone.

The likely large optimization is to avoid waiting for native dirty-page flush
on every ownerless autocommit commit when the committed page images are already
available in `mylite-concurrency.wal`. That cannot be changed safely until the
MTR page-version publisher has measurable coverage evidence: how many modified
pages were candidates, how many were published, and why any candidate was not
published.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes page-version records at
  mini-transaction commit for ownerless page writes that are not deferred to
  the transaction vector. Autocommit SQL normally uses this MTR publication
  path because `ownerless_page_write_uses_transaction_release()` excludes SQL
  autocommit transactions.
- `mariadb/storage/innobase/trx/trx0trx.cc` still treats
  `mylite_ownerless_page_write_trx_id != 0` as a reason to enter the
  ownerless commit visibility bridge, publish tracked transaction pages, wait
  for dirty pages through the commit LSN, and only then release native locks.
- `mariadb/storage/innobase/buf/buf0flu.cc` has a buffer-pool dirty-page
  publisher, but it skips undo pages. A no-flush fast path therefore needs
  evidence from the MTR publisher, not only the buffer-pool publisher, before
  it can claim complete live peer/recovery coverage.
- `packages/libmylite/tests/embedded_performance_probe.c` already reports
  ownerless direct/prepared reads and transactional/autocommit writes. It is
  the lowest-cost place to expose opt-in publication counters next to the
  existing throughput numbers.

## Design

Add disabled-by-default internal counters in the ownerless MTR page-version
publication path. When enabled, count:

- publish candidates,
- successfully published records,
- unpublishable persistent-page filters,
- lock-only transaction skips,
- missing source page image skips,
- missing tablespace metadata skips,
- allocation skips,
- page-LSN/commit-LSN mismatch skips,
- page-version publish hook failures.

Expose internal `extern "C"` reset/read/enable functions from the embedded
InnoDB integration. These are not public `libmylite` API and are used only by
the embedded performance probe.

Extend `mylite_embedded_performance_probe` with
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. When set, the probe enables the
counters only around the ownerless write loops and emits separate transactional
and autocommit page-publish counters. Default probe output remains unchanged.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or ownerless coordination
behavior changes. This is opt-in diagnostics for the performance probe.

## Directory And Lifecycle Impact

No directory-layout change and no new durable state. Counters are process-local
and reset by the performance probe.

## Native Storage Impact

No native storage behavior change. The counters observe the existing
page-version publication path and do not change dirty-page flushing or
page-visible publication.

## Build And Performance Impact

Default runtime behavior and default probe output are unchanged. Ownerless MTR
publication adds one disabled branch before touching process-local atomics. The
branch is limited to the existing ownerless page-version publication path.

When the env flag is enabled, the probe can show whether ownerless autocommit
MTRs publish every modified page image and classify skip reasons. That evidence
will decide whether a later fast path can safely replace the current native
dirty-page flush bridge, or which page classes need more coverage first.

## Test Plan

- Build `mylite_embedded_performance_probe`.
- Run the reduced embedded performance probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and verify transactional and
  autocommit counter keys are printed.
- Run focused ownerless committed-read selectors to prove normal live
  visibility remains unchanged.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08 rebuilt the MariaDB embedded archive after
touching `mariadb/storage/innobase/mtr/mtr0mtr.cc`, then rebuilt
`mylite_embedded_performance_probe`, `mylite_ownerless_cross_process_sql_test`,
and the hook-preset `mylite_ownerless_cross_process_sql_test`.

A reduced stats-enabled probe passed with:

- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`
- `MYLITE_PERF_SELECT_ITERATIONS=50`
- `MYLITE_PERF_INSERT_ITERATIONS=20`

The final post-rebuild run reported ordinary autocommit inserts at
`1929.51 ops/s`, ownerless transactional inserts at `1082.01 ops/s`, and
ownerless autocommit inserts at `101.73 ops/s`. The ownerless transactional
insert loop reported `25` candidates, `25` published records, and zero
skip/failure counts. The ownerless autocommit insert loop reported `120`
candidates, `120` published records, and zero skip/failure counts.

Focused visibility selectors also passed:

- `prepared-committed-read`
- `local-write-first-read`
- `visible-publish-crash`
- `visible-checkpoint-crash`

This proves the simple prepared InnoDB insert workload's MTR page images were
fully available in the page-version WAL, but it does not yet prove broader DML,
DDL, undo, BLOB, compressed, generated-column, or foreign-key page classes.

## Acceptance Criteria

- Counters are disabled by default and enabled only by explicit probe request.
- Probe output distinguishes ownerless transactional and autocommit write-loop
  publication counts.
- Existing focused ownerless visibility coverage still passes.
- The slice documents that this is proof instrumentation, not a production
  fast path.

## Risks And Follow-Up

- Counters prove MTR publication shape for the probe workload, not for every
  possible DML/DDL page class. A production no-flush fast path still needs
  broader SQL coverage before it can replace the conservative bridge.
- If autocommit skip counts are nonzero, the next optimization slice must
  address those page classes or keep the native flush bridge for them.
