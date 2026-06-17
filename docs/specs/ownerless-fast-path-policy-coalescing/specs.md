# Ownerless Fast-Path Policy Coalescing

## Problem

Ownerless statement execution classifies each statement for two related write
fast paths:

- visible-fast commit publication; and
- deferred page-log append batching.

For eligible `INSERT ... VALUES` statements both predicates need the same target
table foreign-key state. The foreign-key result is cached by observed
dictionary generation, but direct and prepared execution still ran the
statement-shape and target-table classification twice at each statement
boundary.

Production probes show startup and reconnect are not the current bottleneck.
The remaining ownerless small-write gap is in statement-boundary and native
write-publication work. This slice removes duplicated first-party policy work
without changing page-version WAL, history proof, checkpoint, or SQL behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` owns the ownerless SQL policy
  classifier around `mylite_exec*()` and `mylite_step()`.
- `ownerless_insert_values_statement_row_count()` parses the bounded
  `INSERT ... VALUES` row-list shape used by the visible-fast and append-batch
  gates.
- `ownerless_insert_target_has_foreign_keys()` resolves the insert target and
  uses `mylite_db::ownerless_insert_foreign_key_cache`, keyed by the observed
  ownerless dictionary generation.
- The previous direct and prepared execution paths called
  `ownerless_statement_allows_visible_fast_path()` and
  `ownerless_statement_allows_append_batch_fast_path()` independently, so an
  eligible insert could repeat the same target classification before entering
  MariaDB execution.

## Design

Introduce a private `OwnerlessStatementFastPathPolicy` result with:

- `visible_fast_path`; and
- `append_batch_fast_path`.

`ownerless_statement_fast_path_policy()` computes both booleans together:

- conservative dictionary-refresh state returns both false;
- `INSERT ... VALUES` row count is parsed once;
- target foreign-key state is resolved once when the insert shape can use
  either fast path;
- visible-fast accepts any non-empty proven row list without foreign keys;
- append batching accepts the existing one-through-four row cap without
  foreign keys; and
- explicit `COMMIT` keeps the existing transaction-scoped visible-fast proof
  and never enables append batching.

## Compatibility Impact

No SQL result, public API, storage format, page-version WAL, native storage,
or unsupported-surface behavior changes. The change coalesces policy
classification only.

## Directory And Lifecycle Impact

No new files, durable records, shared-memory fields, or lifecycle states are
introduced.

## Native Storage Impact

No InnoDB, MyISAM, Aria, redo, undo, checkpoint, or dictionary format changes.
The same ownerless native publication and recovery paths run after
classification.

## Build And Performance Impact

The change is first-party `database.cc` code only. It avoids duplicate
row-list and target foreign-key classification at ownerless statement
boundaries, primarily helping hot prepared/direct `INSERT ... VALUES` loops.
It does not reduce page-version record volume, native history-proof records,
or native commit/checkpoint cost.

## Test And Verification Plan

- Build the production ownerless SQL harness and embedded performance probe.
- Run focused selectors covering:
  - multi-row visible-fast append batching;
  - foreign-key fast-path cache invalidation; and
  - explicit transaction visible-fast commit proof.
- Run reduced production stats-off and stats-enabled performance probes.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Direct and prepared ownerless execution use the combined policy result.
- Foreign-key target classification is shared for eligible insert fast paths.
- Existing visible-fast, append-batch, foreign-key, and explicit-transaction
  coverage passes.
- Performance probes remain valid and do not show an obvious ownerless
  read/write regression.

## Risks And Follow-Up

This is a classification cleanup, not the larger native redo/checkpoint or
history-proof representation work. The dominant remaining write-path targets
stay in native commit/page-publication, page-log encoding, checkpoint
ordering, and proof volume.
