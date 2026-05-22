# Prepared Clean State Fast Path

## Problem

Prepared point-select loops spend most of their remaining overhead above raw
storage lookup in `libmylite` and MariaDB prepared execution. Fresh local
samples show routed prepared primary-key point selects at about `8.3 us/op`,
while storage-level row lookups are about `4.8 us/op` and prepared scalar
execution is about `0.72 us/op`.

Two pieces of MyLite bookkeeping still run on every clean prepared result
iteration even when they have nothing to clear:

- zero-warning result completion clears an already-empty warning vector; and
- statement execution/fetch clears current-row state even when reset or the
  previous fetch has already made the row invisible.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/libmysql.c::mysql_warning_count()` returns
  `MYSQL::warning_count`; the call itself is cheap, but MyLite still clears its
  warning storage after every clean result completion.
- `mariadb/libmysqld/lib_sql.cc` stores embedded statement warning counts in
  `MYSQL::warning_count` after statement execution.
- `packages/libmylite/src/database.cc::capture_warnings()` always calls
  `clear_warnings()` before returning early for `warning_count == 0`.
- `packages/libmylite/src/database.cc::mylite_reset()` marks
  `has_current_row=false`, while `execute_statement()` and
  `fetch_statement_row()` still call `clear_current_row_for_reuse()` before the
  next execution/fetch.

## Design

- Add a private `clear_warnings_if_needed()` helper.
- At direct execution, prepare, prepared execution, and zero-warning result
  completion, clear MyLite warning state only if a previous warning count or
  stored warning rows are still present.
- Make `clear_current_row_for_reuse()` return immediately when no current row
  is visible.
- Keep full warning capture, warning clearing after previous warnings, variable
  result row cleanup, and error paths unchanged.

## Compatibility Impact

Public behavior stays the same. Successful clean direct and prepared statements
still report zero warnings, and a clean statement after a warning-producing
statement still clears the previous warnings.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public API signature or durable file-format change.

## Storage-Engine Routing Impact

No routing change. This is `libmylite` API bookkeeping after MariaDB execution
and before result fetching.

## Build, Size, And Dependencies

Small first-party C++ helper. No new dependency or meaningful binary-size
impact.

## Test Plan

- Build storage-smoke statement, storage-engine, and performance targets.
- Run embedded statement and warning tests.
- Run focused storage-smoke CTest coverage.
- Run the full storage-smoke CTest preset.
- Run prepared point-select, reset-after-row, and scalar-result benchmark
  samples.
- Run `git clang-format --diff` and `git diff --check`.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_embedded_warning_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_warning_test`
- `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-warning|libmylite.embedded-storage-engine'
  --output-on-failure`: 3/3 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 100000`
  - before: `8.296 us/op`
  - after: `7.785 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-reset-after-row 10000 100000`
  - before: `8.171 us/op`
  - after: `7.814 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-scalar-selects 10000 100000`
  - before: `0.720 us/op`
  - after: `0.687 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 10000 1000000`
  - after: `7.800 us/op`
- `git clang-format --diff HEAD -- packages/libmylite/src/database.cc`
- `git diff --check`

## Acceptance Criteria

- Zero-warning result completion avoids no-op warning-vector clearing when
  MyLite already has no stored warnings.
- Clean execution/fetch does not clear invisible current-row state.
- Existing warning tests still prove clean statements clear previous warnings.
- Local benchmarks complete with correct checksums and record the effect.

## Risks And Follow-Up

The expected effect is small because the remaining prepared point-select cost is
dominated by MariaDB prepared execution and storage read setup. Larger gains
still require reducing statement-level storage read scopes or MariaDB prepared
result overhead without breaking read/write interleaving.
