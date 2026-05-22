# Hot Success Status Fast Path

## Problem

Prepared point-select and update loops call successful MyLite APIs repeatedly:
bind, step, reset, then repeat. Several of those success paths call `set_ok()`
even when the owning database handle is already in the OK state. `set_ok()`
rewrites numeric diagnostics and assigns the OK SQLSTATE and message strings,
which is unnecessary in the common no-error loop.

The focused prepared primary-key point-select benchmark now measures about
`9.2 us/op`, giving this small `libmylite` API bookkeeping cost a cheap local
regression target.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party `libmylite` code.
- `packages/libmylite/src/database.cc::set_ok()` assigns `errcode`,
  `extended_errcode`, `mariadb_errno`, `sqlstate`, and `errmsg`.
- Hot prepared loops call `mylite_bind_int64()`, `execute_statement()` through
  `mylite_step()`, and `mylite_reset()` once per iteration.
- Errors are represented by non-OK `errcode` / `extended_errcode`, a nonzero
  MariaDB errno, or both. Current setters keep OK diagnostics consistent when
  those fields are OK.

## Design

- Add a private `set_ok_if_needed()` helper.
- Return immediately when the database diagnostics are already numerically OK:
  `errcode == MYLITE_OK`, `extended_errcode == MYLITE_OK`, and
  `mariadb_errno == 0`.
- Fall back to `set_ok()` whenever a previous error or MariaDB diagnostic needs
  to be cleared.
- Use the helper only on hot, ordinary success paths where repeated OK calls
  are expected: scalar parameter binding, statement reset, and statement
  execution entry.
- Leave explicit error setters, warning capture, close/finalize cleanup, and
  less-hot lifecycle calls unchanged.

## Compatibility Impact

Public API behavior remains the same: successful calls still leave the handle
in the OK state, and a success after an error still clears the previous error.

## Single-File And Embedded Lifecycle Impact

No durable file, storage-engine, journal, lock, recovery, or companion-file
lifecycle change.

## Public API And File-Format Impact

No public API signature or durable file-format change.

## Storage-Engine Routing Impact

No routing change. This is API bookkeeping around MariaDB execution.

## Binary-Size And Dependency Impact

Small first-party C++ helper. No dependency or meaningful binary-size impact.

## Tests And Verification

- Run existing embedded statement and storage-smoke coverage.
- Run focused prepared primary-key point-select and prepared-update benchmarks.
- Run `git diff --check` and `git clang-format --diff` on `database.cc`.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- packages/libmylite/src/database.cc packages/libmylite/tests/embedded_statement_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite.embedded-statement|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.239 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  - Prepared primary-key updates in one transaction: `2.641 us/op`.

## Acceptance Criteria

- Hot successful bind, step execution, and reset paths avoid redundant OK string
  assignment when diagnostics are already OK.
- Success after a previous error still clears diagnostics through `set_ok()`.
- Existing statement and storage-smoke tests pass.
- Local benchmarks record whether the change is measurable.

## Risks And Unresolved Questions

- The helper relies on the existing invariant that numerically OK diagnostics
  imply the OK SQLSTATE and message. If future code introduces extended warning
  states with `MYLITE_OK`, it should either keep that invariant or avoid this
  helper.
