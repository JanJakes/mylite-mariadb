# Ownerless MTR Fast Path Audit

## Problem Statement

After the page-log delta and sparse payload direct-copy slices, the reduced
production ownerless insert probe still showed measurable time inside
`mtr_t::commit_log()` and the ownerless page-write wrapper. A fresh 200-row
stats-enabled production sample at `1a48c20e` reported stable ownerless
autocommit publication counts (`800` page-write publish calls, `3.000` page
versions per insert, `1.000` native-support elided pages per insert, `2.000`
native-support published pages per insert) with `page_write_leave_total_ms`
at `2.096`, `page_write_commit_log_publish_ms` at `22.408`, and
`page_write_commit_log_no_dirty_loop_ms` at `13.159`.

The next candidate micro-optimization was to reduce repeated ownerless MTR
bookkeeping without changing page-version WAL, native storage, or SQL
semantics. This audit records why that candidate was rejected.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::ownerless_page_writes_publish()`
  calls `ownerless_page_write_uses_transaction_release()` for each modified
  MTR slot before deciding whether to capture a transaction-deferred page image
  or publish immediately.
- The no-dirty `mtr_t::commit_log()` release loop performs the same
  per-modified-slot predicate check.
- `ownerless_page_write_leave()` checks whether the page latch slot is tracked
  in `m_ownerless_page_write_mtr_pages`, then calls
  `ownerless_page_write_forget_mtr_page()`, which packs the same page identity
  and searches the same vector again before erasing it.
- The apparent optimization was to cache the transaction-release predicate for
  the MTR publish pass and use the first tracked-page lookup for erase.

## Prototype Results

Two local prototypes were built and tested, then fully backed out:

- The first prototype cached the transaction-release predicate per publish
  pass and changed tracked-page removal to unordered erase. Focused production
  and hook selectors passed, and one ownerless stress run passed. Later stress
  reruns failed: the first stress case observed a reader total decrease
  (`previous=239`, `current=237`), and the DDL stress case hit the InnoDB
  assertion `trx->error_state == DB_SUCCESS` at
  `mariadb/storage/innobase/trx/trx0trx.cc:1345`.
- The narrowed prototype preserved the original ordered
  `small_vector::erase()` behavior while still caching the publish predicate
  and avoiding the second lookup. It also failed the first ownerless stress
  case after cleanup and isolated rerun, with reader totals decreasing
  (`previous=386`, `current=372`).

After backing out the prototype and rebuilding the MariaDB embedded archive
from the restored source, the isolated first ownerless stress case passed in
`45.04s`.

## Decision

Do not land this MTR fast path. The visible code reduction is too small to
justify a change that can perturb ownerless visibility stress, and the failures
occurred in the same cross-process monotonic-read surface that the ownerless
protocol is meant to protect.

Future MTR wrapper optimization needs a stronger proof than "same apparent
predicate inside one mini-transaction." In particular, any attempt to cache
`ownerless_page_write_uses_transaction_release()` or alter tracked-page release
must explain interaction with transaction state, page-write gate ownership,
transaction-deferred publication, DDL stress, and reader snapshot refresh
before implementation.

## Compatibility Impact

No product code is changed by this audit. The ownerless runtime remains at the
previously committed behavior from `1a48c20e`.

## Test And Verification Record

Local commands run during the audit:

- `tools/mariadb-embedded-build build` rebuilt the `MinSizeRel` MariaDB
  embedded archive for the prototype and again after backing it out.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed for the prototype.
- Focused production selectors passed for the prototype:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$|^libmylite\.ownerless-uncommitted-peer-hidden$'
  --output-on-failure`.
- Focused hook selectors passed for the prototype:
  `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)$'
  --output-on-failure`.
- Prototype stress reruns failed as described above, so the code was backed
  out.
- After restoring the previous source, rebuilding, and cleaning aborted
  `/tmp/mylite-ownerless-sql.*` directories, the isolated baseline stress case
  passed:
  `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`.

## Acceptance Criteria

- No MTR fast-path code remains in the worktree.
- The rejected optimization and failure signatures are recorded so the next
  performance slice does not repeat this path without a stronger correctness
  proof.
- Restored-source ownerless stress can run the first monotonic-reader stress
  case successfully after rebuilding from the backed-out source.
