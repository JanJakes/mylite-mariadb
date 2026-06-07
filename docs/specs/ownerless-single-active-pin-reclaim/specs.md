# Ownerless Single Active Pin Reclaim Deferral

## Problem

The page-log primitive can distinguish a single active snapshot from a
multi-pin case and can drop checkpointed post-snapshot records when all
snapshot-sensitive pages have boundary proof. Earlier planning treated that
primitive as the next product optimization for close-time reclaim with exactly
one live page-version pin.

Current product code intentionally does not take that optimization. A writable
ownerless runtime can force native InnoDB checkpoint side effects during
reclaim. Doing that while another process still owns a live snapshot runtime
can expose redo-header and startup ordering that is not yet safe enough for the
product path. The implemented policy therefore retains page-version WAL while
any live page-version pin exists, even if a single-pin primitive could compact
some records.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:reclaim_ownerless_page_log_after_native_checkpoint()`
  snapshots the page-version pin registry and returns while live peers have
  any active pin. The code comment records that native checkpointing at this
  point can rewrite redo state that a concurrent ownerless startup must read.
- `packages/libmylite/src/ownerless_page_log.cc` still implements
  `mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at()` and
  `mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at()`.
  Primitive coverage proves both retention modes.
- `packages/libmylite/src/database.cc:publish_ownerless_snapshot_boundary_if_needed()`
  can synthesize a boundary record while a pin is live. Those records support
  active readers and post-release cleanup, but they do not authorize product
  native checkpoint reclaim while the pin remains active.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` covers the
  product behavior through `live-reclaim` and
  `active-pin-reclaim-boundary`: active pins retain WAL until release,
  synthesized/retained boundaries remain available to the pinned reader, and
  no-live or zero-pin reclaim checkpoints retained WAL after release.

## Scope And Non-Goals

In scope:

- Align this spec with the implemented conservative product policy.
- Keep the single-snapshot primitive documented as lower-level evidence rather
  than an enabled product path.
- Keep pressure diagnostics, soft write limits, timer scheduling after pin
  release, and native boundary synthesis as the current mitigations for
  long-lived active readers.

Out of scope:

- Enabling native checkpoint reclaim while any live page-version pin remains.
- Changing page-log primitive semantics.
- Adding background page-aware pruning or group commit.
- Claiming full external MariaDB/RQG pressure coverage.

## Compatibility Impact

SQL isolation behavior is unchanged. Repeatable-read and serializable readers
continue to read the page-version WAL for their pinned LSN while native files
advance. Writers may continue to grow retained WAL under long-lived readers
unless an application opts into `ownerless_page_log_limit_bytes`.

## Directory And Lifecycle Impact

No directory layout change. The conservative policy keeps
`concurrency/mylite-concurrency.wal` as the active snapshot authority until the
pin releases. After release, existing zero-pin live-peer or no-live reclaim
uses native checkpoint proof before truncating retained records.

## Native Storage Impact

Native InnoDB checkpointing remains part of reclaim proof, but product runtime
close does not force it while a live page-version pin remains. This avoids
mixing active snapshot runtime state with redo-header and native-startup
ordering that still needs a separate design before active-pin product reclaim
can be enabled.

## Test Plan

- Run the focused `live-reclaim` selector in `embedded-dev`.
- Run the focused `active-pin-reclaim-boundary` selector in
  `ownerless-test-hooks`.
- Run the ownerless primitive tests that cover oldest-snapshot and
  single-snapshot page-log retention.
- Run `git diff --check`.

## Acceptance Criteria

- Docs no longer claim product close-time reclaim uses the single-active-pin
  primitive.
- Product behavior remains: any live active page-version pin retains WAL until
  release.
- Primitive single-snapshot compaction remains documented and tested as
  lower-level evidence for a future safe active-pin reclaim design.

## Risks And Follow-Up

- Long-lived active readers can still retain WAL. Existing mitigations are the
  opt-in pressure limit, pressure diagnostics, statement/timer checkpointing
  after pins release, and external deterministic pressure traces.
- Future active-pin product reclaim needs a source-backed redo/startup safety
  design before the single-snapshot primitive can be wired into close-time
  reclaim.
