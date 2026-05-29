# Dirty-Page Buffer Merge Future-Current 32-63 Direct Write Experiment

## Problem

After the committed `16-31` future-current direct-write policy, the remaining
dirty leaf pressure admissions are still merge-sourced and split across
`32-63`, `64-127`, and `128+` free-slot ranges. The next nearest candidate was
to direct-write `32-63` free-slot future-current leaves, but the earlier broad
all-partial direct-write experiment regressed prepared insert to
`94.432 us/op`. This slice records the bounded `32-63` experiment and why it
was not adopted.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The experiment changed first-party MyLite storage policy in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB handler or SQL
  source was involved.
- `mylite_storage_commit_statement()` advances the parent current header
  before merging child dirty pages, and rollback truncates the file back to the
  stable parent header page count. Future-current pages below the current
  header and beyond the stable header remain rollback-protected by truncation.
- The committed direct-write guard requires a full parent dirty buffer, a
  future-current page below the parent current header page count, no parent or
  child append-buffer residency, an index-leaf page, and no parent
  dirty-buffer resident entry.
- The committed `16-31` policy reports `34,484`
  `future-current-header-partial-leaf` fallback rows, split into `18,349`
  with `32-63` free slots, `14,152` with `64-127`, and `1,983` with `128+`.

## Experiment

The local experiment added a `future-current-header-32-63-direct-write` guard
outcome and routed `32-63` free-slot future-current index leaves through the
existing `direct_write_dirty_page_buffer_merge_entry()` path. Full leaves,
`1-15` leaves, and `16-31` leaves kept their committed direct-write outcomes.
Leaves with `64+` free slots stayed on fallback replay.

The focused future-current direct-write storage self-test was extended locally
to cover a `63` free-slot page. The experiment built successfully and the
storage-smoke prepared-insert component benchmark ran successfully, but the
benchmark result was not good enough to keep the behavior.

## Benchmark Evidence

Compared with the committed `16-31` policy evidence (`68.775 us/op`,
`53,136` dirty leaf direct writes, and `34,484` dirty leaf pressure
admissions), the `32-63` experiment reported:

- `72.554 us/op` for the prepared insert step.
- `76,001` dirty `index-leaf` merge direct writes.
- `22,483` `future-current-header-32-63-direct-write` rows.
- `15,263` remaining dirty `index-leaf` pressure admissions, all with `64+`
  free slots.
- Leaf growth fast replacements dropped from `33,851` to `30,199`.
- The MariaDB smoke archive was `33,974,202` bytes (`32.40 MiB`).

## Decision

Do not adopt the `32-63` direct-write policy. It reduces dirty leaf pressure
admissions, but the prepared insert step regresses from the committed
`16-31` policy and loses additional in-buffer leaf growth coalescing.

Keep `32+` free-slot future-current leaves on fallback replay until a
different design can preserve coalescing or prove a better threshold with
fresh benchmark evidence.

## Compatibility And Lifecycle Impact

No committed SQL, C API, handler API, metadata, storage-engine routing,
file-format, or embedded lifecycle behavior changes. The durable storage model
remains the primary `.mylite` file plus the existing MyLite-owned journal
lifecycle.

## Risks And Follow-Up

- The benchmark is local VPS evidence, not a universal performance proof, but
  it is enough to reject this threshold for the current roadmap path.
- A future design could revisit broader partial-leaf publication if it can
  preserve parent dirty-buffer coalescing or make the direct-write cost
  conditional on later rewrite probability.
