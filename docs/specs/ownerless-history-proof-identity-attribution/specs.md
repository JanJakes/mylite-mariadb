# Ownerless History Proof Identity Attribution

## Problem

The history-proof delta attribution slice used a four-slot same-page image
cache and reported high eviction rates. That showed the cache was too small for
the simple insert proof-page stream, but it did not distinguish two very
different cases:

- proof pages truly churn through mostly new rollback-segment and undo-header
  identities; or
- proof page identities repeat often, but collide out of the tiny full-page
  diff cache before a same-identity comparison can happen.

A future delta WAL format only helps the current write path if proof identities
repeat often enough. MyLite therefore needs identity-level attribution that is
larger than the full-page diff cache without adding megabytes of diagnostic
page storage.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc::
  trx_t::write_serialisation_history()` records the ownerless history-proof
  rollback-segment and undo-header page identities for the active history
  mini-transaction.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::
  ownerless_page_write_history_proof_roles()` maps an accepted page image to
  the active proof roles.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::
  ownerless_page_publish_count_history_proof_role_diff()` stores full page
  images in a four-slot direct-mapped cache per proof role so it can count byte
  diffs for same-page repeats.
- The same file already has a low-memory `ownerless_page_publish_count_identity()`
  helper for all page-version publications. It uses a larger fingerprint table
  and counts unique, duplicate, and overflow outcomes without storing page
  images.

## Design

Append diagnostics-only identity counters for accepted history-proof pages:

- rollback-segment proof identity unique, duplicate, and table-overflow counts;
- undo-header proof identity unique, duplicate, and table-overflow counts.

Each role gets a process-local 1024-slot open-addressed table of 64-bit
fingerprints. The fingerprint is derived from `(space_id, page_no, page_size)`;
it deliberately ignores commit LSN so a repeated proof page identity is counted
as a duplicate even when it is published at a later commit boundary.

The new counters run only under the existing page-publish stats flag and only
for accepted proof-role publications. They do not change page-version WAL
format, page publication, recovery, or checkpoint behavior.

## Scope And Non-Goals

In scope:

- per-role identity attribution for accepted history-proof pages;
- raw and per-insert performance-probe output;
- focused SQL assertions that identity unique + duplicate + overflow equals
  each role's accepted proof samples;
- docs that update the next proof-representation target.

Out of scope:

- replacing the history-proof page images;
- adding a delta WAL record format;
- changing native redo/checkpoint reconciliation;
- broad DDL/file lifecycle recovery or external MariaDB/RQG stress.

## Compatibility Impact

No SQL, public C API, PHP/mysqli, wire-protocol, storage-engine, storage-format,
or directory-layout behavior changes. This is internal diagnostics only.

## Directory And Lifecycle Impact

No durable or shared-memory files are added. The fingerprint tables are
process-local and reset by `mylite_ownerless_innodb_reset_page_publish_stats()`.

## Native Storage Impact

No native InnoDB page, redo, undo, checkpoint, or recovery behavior changes.
The current history proof still requires accepted page-version WAL records for
the rollback-segment and undo-header pages before the native exact history
flush can be skipped.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt after editing `mtr0mtr.cc`. The
diagnostic tables add 2048 64-bit atomics plus six counters to process-local
state. The normal stats-disabled path adds only the existing stats-enabled
branch checks; no new durable work is performed.

The reduced production stats-enabled probe after this slice reported:

- autocommit rollback-segment proof samples: `1.000` per insert;
- autocommit rollback-segment proof identity unique: `1.000` per insert;
- autocommit rollback-segment proof identity duplicate: `0.000` per insert;
- autocommit rollback-segment proof identity overflow: `0.000` per insert;
- autocommit undo-header proof samples: `1.000` per insert;
- autocommit undo-header proof identity unique: `1.000` per insert;
- autocommit undo-header proof identity duplicate: `0.000` per insert;
- autocommit undo-header proof identity overflow: `0.000` per insert;
- bulk insert rollback-segment/undo proof identities: `24` unique and `1`
  duplicate for each role across `25` proof samples.

This proves the simple autocommit insert hot path is dominated by fresh proof
page identities, not repeated same-page proof images hidden by the four-slot
diff cache. A simple same-page delta WAL format is therefore not the next
runtime optimization for that path.

## Test And Verification Plan

- Rebuild the production MariaDB embedded archive.
- Rebuild production PHP-embedded ownerless SQL, primitive, and performance
  probe targets.
- Run focused history proof and native-support page WAL elision SQL selectors.
- Run ownerless primitive page-log coverage as a readback guard.
- Run a reduced stats-enabled production performance probe and record
  proof-identity unique/duplicate/overflow evidence.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Focused SQL tests assert that identity unique + duplicate + overflow equals
  accepted proof samples for both proof roles.
- Probe output emits raw and per-insert identity counters.
- The reduced attribution probe distinguishes fresh proof identity churn from
  same-page proof reuse.
- No WAL format, native storage, or compatibility behavior change is
  introduced.

## Risks And Unresolved Questions

- This is attribution, not a runtime speedup.
- A fingerprint table can overflow under broader workloads; overflow counters
  make that visible.
- The next performance design should target why simple inserts allocate or
  publish fresh proof identities, or replace the proof with broader native
  redo/checkpoint evidence, rather than relying on same-page deltas.
