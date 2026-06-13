# Ownerless Index Page Delta Attribution

## Goal

Measure whether remaining `FIL_PAGE_INDEX` ownerless page-log payload is
dominated by repeated publication of the same page identities with small
changed-byte deltas. This gives the next WAL-format optimization concrete
evidence instead of guessing after fill-sparse compression proved ineffective
for representative insert index pages.

## Non-Goals

- Do not change the ownerless page-log durable format or replay behavior.
- Do not add a delta page-log record type in this slice.
- Do not change ownerless page-version visibility, snapshot retention,
  checkpoint policy, native redo/checkpoint semantics, or DDL/file lifecycle.
- Do not claim ownerless concurrency complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_TYPE` at
  offset 24 and `FIL_PAGE_INDEX` as the uncompressed B-tree page type.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` updates `FIL_PAGE_LSN` during
  mini-transaction commit, then MyLite publishes ownerless page images from
  the committed buffer-pool frame.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` already has ownerless page-publish
  identity counters using a fixed-size fingerprint table to classify unique
  versus duplicate published page identities.
- `mariadb/storage/innobase/buf/buf0flu.cc` uses the same fixed-size
  fingerprint-table style for ownerless flush identity attribution.
- `packages/libmylite/src/ownerless_page_log.cc` owns page-log payload
  encoding and already attributes append counts, payload bytes, compact-sparse
  metadata/data bytes, and page types for the stats-enabled embedded
  performance probe.
- The preceding `ownerless-index-fill-sparse-prefilter` sample reported
  `1779.030` index payload bytes per simple ownerless autocommit insert while
  fill-sparse selected only the SYS proof page. The remaining index bytes need
  a stronger representation than repeated-fill compression.

## Compatibility Impact

No SQL, C API, mysqli, WordPress, wire-protocol, or durable page-log behavior
changes. The slice adds internal diagnostic counters emitted by test/probe
binaries when ownerless page-log append stats are enabled.

## Design

Extend the ownerless page-log append stats path with an index-page attribution
table keyed by `(space_id, page_no)`. The table is process-local diagnostic
state, reset with the existing page-log append stats, and consulted only when
append stats are enabled.

For each appended `FIL_PAGE_INDEX` image:

- if the page identity has not been seen, count it as unique and store a copy
  of the page image in the diagnostic slot;
- if the identity is already present with the same page size, count it as a
  duplicate sample, compare the new page image against the previous image,
  record total changed bytes, split changed bytes into `FIL` header bytes and
  non-header body bytes, then replace the stored image with the new image;
- if the identity exists with a different page size, count the sample as a size
  mismatch and replace the stored image;
- if the fixed table cannot place the identity within the bounded probe limit,
  count an overflow and do not allocate fallback state.

The table must not run on stats-disabled production paths. It may keep full
page images because it is a diagnostic-only path enabled by explicit
performance probes, and it avoids changing native InnoDB code while measuring
the page-log representation question directly.

## File Lifecycle

No file lifecycle change. Diagnostic state is process memory only. Durable
state remains the existing ownerless page-version WAL inside the MyLite
database directory.

## Embedded Lifecycle And API

No public API, directory-open, close, or embedded runtime behavior changes.
The internal stats reset function clears the diagnostic table so repeated probe
phases do not leak attribution across ordinary, ownerless, bulk, or focused
samples.

## Build, Size, And Dependencies

No dependency or build-profile change. The added diagnostic table lives in the
first-party page-log module and is compiled into existing test/probe-capable
builds. It increases binary/data size modestly for internal instrumentation,
but does not add external libraries or public ABI.

## Test Plan

- Extend ownerless primitive coverage to prove:
  - repeated `FIL_PAGE_INDEX` appends for the same `(space_id, page_no)` count
    duplicate samples,
  - changed-byte, header-byte, and body-byte counters match known page edits,
  - unique, duplicate, and size-mismatch accounting matches index append
    counts in the deterministic sample,
  - non-index page appends do not affect index-delta counters.
- Emit the new counters from the embedded performance probe in both raw
  page-log append stats and per-insert ownerless autocommit summaries.
- Run focused production primitive and ownerless SQL selectors.
- Run the reduced stats-enabled production performance probe and record the
  measured index duplicate rate and changed-byte density.
- Run hook ownerless coverage, reduced ownerless stress, production-build
  guards, formatter, and whitespace checks.

## Acceptance Criteria

- Existing page-log encoding/replay behavior remains byte-exact.
- Focused ownerless primitive and SQL selectors pass.
- The stats-enabled production probe reports index identity unique,
  duplicate, size-mismatch, overflow, changed-byte, `FIL` header changed-byte,
  and body changed-byte summaries per ownerless autocommit insert.
- The docs state whether a future index delta WAL format is supported by the
  observed duplicate rate and changed-byte density.

## Risks And Open Questions

The diagnostic table copies full index page images while stats are enabled, so
it must remain gated behind append stats and reset between measurements. The
bounded table can overflow on broad workloads; overflow is reported as a
counter so a low duplicate rate is not mistaken for proof that duplicates do
not exist. This slice answers whether a delta format looks promising; it does
not itself reduce ownerless write cost.

## Implementation Evidence

The page-log append stats path now keeps a bounded, stats-only diagnostic table
for `FIL_PAGE_INDEX` identities. The primitive test appends the same index page
identity with two same-size deltas and one size-mismatch sample, verifies the
known `5` changed bytes split into `2` `FIL` header bytes and `3` body bytes,
and confirms a non-index SYS append does not affect the index counters. The
embedded performance probe emits raw append counters and per-insert ownerless
autocommit summaries, and fails if index identity accounting does not add up
to index append count or if header/body changed-byte buckets do not add up to
the changed-byte total.

A final reduced 100-row stats-enabled production probe under
`php-embedded-prod` reported ordinary autocommit at `1890.87 ops/s`,
ownerless autocommit at `634.28 ops/s`, page-log append at `0.168 ms/insert`,
append encode at `0.100 ms/insert`, and unchanged remaining payload
dominance: `1779.010` index bytes, `159.340` undo-log bytes, and `72.030` SYS
bytes per insert. The new index identity counters reported `0.020` unique index
identities, `0.990` duplicate index identities, no size mismatches or
overflows, and only `39.860` changed bytes per insert, split into `1.540`
`FIL` header bytes and `38.320` body bytes. The bulk insert phase showed the
same `2` unique and `99` duplicate index identities across `101` index records.

That evidence supports a bounded follow-up index delta WAL format: the current
representative index payload writes about `1779` bytes per insert for page
images whose consecutive same-identity change set is about `40` bytes.
