# Active Checkpoint Write Amortization

## Problem

Routed SQL inserts and index rebuilds still pay storage work that SQLite-like
file engines avoid on hot write paths. Within an active MyLite statement or
transaction, each append-style mutation was allowed to start and finish its own
recovery journal and publish a new header page immediately. Unique-key checks
also re-read the durable exact-index state for repeated non-nullable fixed-width
keys inside the same active transaction.

That preserves correctness, but it makes explicit-index rebuilds and duplicate
checks spend most of their time in repeated file publication and scans instead
of appending useful row or index pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB drives MyLite row-DML rollback through handler transaction and
  savepoint hooks. MyLite maps those hooks to first-party statement checkpoints
  in `mariadb/storage/mylite/ha_mylite.cc`.
- MyLite storage already snapshots statement-start header/catalog pages and has
  separate active transaction journals. The storage layer can therefore keep a
  single active recovery journal for ordinary statement writes and defer header
  publication until checkpoint commit without changing SQL-visible semantics.
- MyLite's guarded raw exact-key path is currently limited to shapes where
  MariaDB key bytes are safe to compare directly. That same guard can be reused
  for duplicate-key probes before falling back to broad index-entry scans.

## Design

- Add an active write-journal wrapper around storage mutations.
  - Outside an active checkpoint, writes keep the existing per-mutation recovery
    journal behavior.
  - Inside an active statement, the first mutation starts one recovery journal
    covering header and catalog state; later mutations reuse it.
  - Active durable transactions continue to use their transaction journal and
    do not create a normal `-journal` companion for each row.
- Keep the mutable checkpoint header in memory.
  - Header page reads from the active writer return the statement-local current
    header.
  - Header page writes update the statement-local current header and mark it
    dirty instead of rewriting page `0` immediately.
  - Checkpoint commit publishes the header to the parent checkpoint or to disk.
  - Checkpoint rollback restores the saved header/catalog, preserves required
    autoincrement rollback gaps, truncates to the restored header, and then
    propagates the resulting current header to the parent checkpoint when one
    exists.
- Cache exact index probes on the outer active checkpoint.
  - An outer active statement or transaction can build an in-memory exact-index
    cache from live durable index entries for one table/index/key-size tuple.
  - Successful inserts append new matching key/row-id pairs to loaded caches.
  - Nested statement checkpoints use the outer cache so libmylite's
    per-statement rollback checkpoints do not force a full exact-index scan for
    every inserted row.
  - Update, delete, truncate, catalog writes, and nested checkpoint rollbacks
    invalidate active exact-index caches.
  - Nested savepoint release keeps the cache; nested savepoint rollback clears
    parent caches before the restored header is propagated.
- Use storage-level exact lookup for guarded duplicate-key checks in the handler
  before falling back to the existing entryset scan.

## Compatibility Impact

No SQL-visible behavior changes are intended. Statement rollback, transaction
rollback, savepoint release/rollback, autoincrement gap preservation, and
duplicate-key error behavior remain the compatibility contract.

The guarded duplicate-key fast path is only used for non-nullable raw
fixed-width unique-key shapes. Nullable, prefix, collation-sensitive, composite,
or otherwise unsupported key shapes keep the existing fallback path.

## Single-File And Lifecycle Impact

No durable file-format change. Active file-backed statements may keep one normal
MyLite recovery journal open until checkpoint commit or rollback. Active durable
transactions continue to use the transaction-journal companion instead of a
normal recovery journal for each row append.

The primary `.mylite` file remains the only durable database asset. Journals are
MyLite-owned lifecycle companions and are removed after successful checkpoint
completion.

## Public API And File-Format Impact

No public C API or on-disk format change. The change is internal to first-party
storage and the MariaDB handler's duplicate-check routing.

## Storage-Engine Routing Impact

Routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and omitted-engine
durable tables benefit because they already resolve to the MyLite handler and
storage layer. Runtime-volatile MEMORY/HEAP duplicate checks use the equivalent
volatile exact-entry helper but do not participate in durable header or journal
publication.

## Tests And Verification

- Extend storage checkpoint tests to assert the normal recovery journal exists
  while a file-backed statement is active and is removed after statement commit
  or rollback.
- Assert active durable transactions do not create a normal recovery journal for
  ordinary row appends.
- Run storage unit tests, storage-engine compatibility coverage, the local
  performance baseline, formatting, and whitespace checks.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build build/mariadb-mylite-storage-smoke --target mylite_se mysqlserver`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Direct inserts in one transaction: `190.450 us/op`.
  - Prepared inserts in one transaction: `185.285 us/op`.
  - Prepare secondary leaf benchmark rows: `206.192 us/op`.
  - Publish secondary leaf index: `397.811 ms`.
  - Direct ordered full scan: `83.899 us/op`.
- `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c mariadb/storage/mylite/ha_mylite.cc packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

## Acceptance Criteria

- Active statements amortize normal recovery-journal creation across the
  checkpoint instead of per mutation.
- Active statements and transactions defer repeated header-page writes until
  checkpoint boundaries while preserving rollback and savepoint behavior.
- Guarded duplicate-key checks can use exact storage probes without broad
  entryset scans, including inside libmylite's nested per-statement checkpoints.
- The storage-engine compatibility harness passes, including native handler
  savepoint coverage.
- The local performance baseline records a material improvement in explicit
  fixed-width index publication.

## Risks

- This is not the final SQLite-like pager or index design. It removes avoidable
  checkpoint and duplicate-check work, but routed SQL inserts are still measured
  in milliseconds per row on the current local harness.
- The active exact-index cache is intentionally narrow. Broader key shapes need
  collation-aware or encoded-key-safe cache rules before they can use this path.
- Deferring header writes makes nested checkpoint propagation sensitive. The
  savepoint compatibility tests are the current guardrail, and future storage
  concurrency work should add stronger crash/recovery coverage around active
  transaction journals.
