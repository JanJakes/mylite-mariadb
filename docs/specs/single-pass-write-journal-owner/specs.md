# Single-Pass Write Journal Owner

## Problem

Prepared row updates call `begin_write_journal_for_statement_pages()` for every
successful storage mutation. When an active statement or transaction journal
already exists, the helper first scans the statement chain to see whether any
write journal exists, then scans again for a transaction journal, then the
transaction/recovery journal protection helper scans again to find the owner.

The sampled prepared-update profile still shows
`begin_write_journal_for_statement_pages()`,
`statement_chain_has_write_journal()`,
`statement_chain_has_transaction_journal()`, and
`transaction_journal_owner_in_statement_chain()` under the hot update path.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `update_row_with_index_entries()` passes the already-resolved active
  statement to `begin_write_journal_for_statement_pages()`.
- `begin_write_journal_for_statement_pages()` owns the active mutation decision:
  reuse an existing transaction journal, reuse an existing recovery journal, or
  create a new recovery journal.
- `ensure_existing_journal_protects_pages()` already accepts the resolved
  journal owner; the repeated scans are only in the caller-side owner decision.

## Design

- Add one helper that walks the statement chain once and returns the first
  recovery-journal owner and first transaction-journal owner.
- In `begin_write_journal_for_statement_pages()`, use that helper to decide the
  active journal path:
  - transaction journal owner wins when present, preserving existing behavior;
  - otherwise recovery journal owner is used when present;
  - otherwise a new recovery journal is created as before.
- Keep the existing transaction/recovery journal helper wrappers for other
  pager paths that do not need this combined decision.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same journal owner is selected, but the hot begin path
does fewer statement-chain walks.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle changes. Recovery and transaction
journal creation, rewrite, flush, and cleanup semantics are unchanged.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run the prepared-update benchmark when unrelated machine load is low enough
  for meaningful timing.

## Acceptance Criteria

- Active write-journal begin logic resolves recovery and transaction journal
  owners in one statement-chain pass.
- Transaction journal preference is unchanged when both owner types are present.
- Existing rollback, recovery, transaction, and embedded storage-engine tests
  pass.

## Risks

- A wrong owner selection would weaken rollback or crash recovery. The helper
  preserves the previous preference order and only bypasses repeated owner
  rediscovery in the begin path.
