# Handler Active Checkpoint Preference

## Problem

Prepared direct-update sampling still shows
`mylite_storage_borrow_active_statement()` under handler target-row reads and
row updates. When the handler owns the statement or transaction checkpoint, it
can validate and reuse the THD context pointer directly. When the outer
`libmylite` checkpoint owns the active statement, the handler can cache a
non-owning pointer on the handler during statement-lock setup instead of
rediscovering it in both the target-row read and row update paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::external_lock()` begins the statement
  or transaction checkpoint and stores it in `Mylite_trx_context`.
- `ha_mylite::read_exact_unique_index_row_into()` previously called
  `mylite_storage_borrow_active_statement(primary_file)` before checking the
  handler THD context.
- `ha_mylite::update_row()` used the same borrow-first order before falling
  back to `Mylite_trx_context::statement` or `transaction`.
- `packages/mylite-storage/src/storage.c::active_statement_for()` walks the
  storage active-statement parent chain and compares filenames.
- `mylite_storage_statement_matches()` validates a handler-owned THD-context
  statement against the current primary file before trusting the pointer.
- In the prepared-update benchmark, the active statement is owned by the outer
  `libmylite` checkpoint. `ha_mylite::external_lock()` observes it as already
  active and does not create a handler-owned `Mylite_trx_context::statement`.

## Design

- Prefer the current THD's active statement checkpoint when it matches the
  current primary `.mylite` filename.
- Cache the resolved active statement as a non-owning handler hint during
  `external_lock(F_WRLCK)`.
- Remember the primary-file pointer used for that hint. Hot row read/update
  calls can reuse the cached pointer directly when the current primary-file
  pointer is identical, and fall back to storage rediscovery otherwise.
- Clear the handler hint on non-write `external_lock()`, `close()`, and
  `reset()` without committing or rolling it back.
- Fall back to the THD transaction checkpoint when it matches the same file.
- Only call `mylite_storage_borrow_active_statement()` when the handler context
  does not already provide a matching storage statement.
- Apply this order to both accepted exact-index target-row reads and row update
  writes.
- Keep generic index reads, volatile rows, fallback filename-scoped storage
  APIs, and all checkpoint ownership rules unchanged.

## Affected Subsystems

- MyLite MariaDB storage-engine handler.
- Prepared direct-update row read and row mutation hot paths.

## Compatibility Impact

No SQL-visible behavior, affected-row behavior, warnings, diagnostics,
transaction behavior, or storage-engine routing should change. The handler only
uses an owned checkpoint pointer after validating that it belongs to the same
primary `.mylite` file. Handler hints are populated only after statement-lock
setup resolves the active checkpoint and reused only while the current
primary-file pointer is identical. Borrowed pointers never transfer checkpoint
ownership to the handler.

## Single-File And Embedded Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. The slice only changes how the handler selects an already-active
statement pointer for immediate storage calls, and handler hints are cleared
when the handler unlocks or resets.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Tiny handler branch reordering. No dependency change and no expected meaningful
archive-size impact beyond ordinary noise.

## Tests And Verification Plan

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`
- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine CTest coverage.
- Run the full `storage-smoke-dev` CTest preset.
- Run `prepared-update-components` and capture a delayed steady-loop sample.

## Acceptance Criteria

- Prepared direct updates still pass existing routed storage tests.
- Handler target-row reads and row updates use the matching owned or borrowed
  THD checkpoint before asking storage to rediscover an active statement.
- The steady prepared-update sample reduces
  `mylite_storage_borrow_active_statement()` from the handler hot path.

## Risks And Unresolved Questions

- The handler must not trust a stale or different-file THD context pointer.
  Owned THD-context pointers are therefore gated by
  `mylite_storage_statement_matches()`, and handler hints are reused only when
  the current primary-file pointer matches the pointer recorded with the hint.
- Borrowed active statements must remain non-owning. Handler cleanup clears the
  pointer without calling storage commit or rollback on it.
- The borrowed pointer fast path relies on the stable `mylite_primary_file_path()`
  pointer used by the storage-smoke embedded profile. If a future integration
  supplies equivalent filenames through different pointers, the helper falls
  back to storage rediscovery.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`: passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed,
  `libmariadbd.a` is 21,188,560 bytes / 20.21 MiB / 481 members.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: bind `0.024 us/op`,
  step `2.028 us/op`, reset `0.023 us/op`.
- Delayed steady-loop sample:
  `sample mylite_perf_baseline 1 -wait -mayDie -file
  /tmp/mylite-handler-active-hint.sample.txt` while running
  `prepared-update-components 10000 1000000`. The sample no longer shows
  `mylite_storage_borrow_active_statement()` under
  `ha_mylite::read_exact_unique_index_row_into()` or `ha_mylite::update_row()`;
  remaining samples are under `ha_mylite::external_lock()` setup. The same
  sample still shows `rewrite_active_update_pages()`,
  `find_exact_index_row_id()`, `read_indexed_row_payload_from_open_file()`,
  `JOIN::prepare()`, and `open_tables()` as remaining work.
