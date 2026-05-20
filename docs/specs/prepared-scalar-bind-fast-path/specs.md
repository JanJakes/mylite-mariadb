# Prepared Scalar Bind Fast Path

## Problem

Prepared statement reset now keeps bindings for reuse, and repeated same-type
scalar binds avoid rebinding the MariaDB `MYSQL_BIND` array. The public scalar
bind calls still reset the whole `BoundValue` object before storing the new
number, even when the old value has the same scalar type and the MariaDB bind
array already points at the same in-object storage.

The current prepared-update benchmark calls `mylite_bind_int64()` once per row
update. That cost is smaller than MariaDB's per-execute planning cost, but it is
still in the sampled path and is part of SQLite-style reset-and-rebind loops.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party `libmylite` API work in
  `packages/libmylite/src/database.cc`; no upstream MariaDB file changes.
- `mylite_bind_int64()`, `mylite_bind_uint64()`, and `mylite_bind_double()`
  mark parameter binds dirty only when the value kind changes.
- `bind_statement_parameters()` returns without calling
  `mysql_stmt_bind_param()` when the existing bind array is already bound and
  not dirty.
- For same-kind scalar rebinds, the existing MariaDB bind array points at the
  same `BoundValue` scalar member and `mysql_is_null` flag. Updating those
  fields is enough; object-wide reset work is redundant.

## Design

- Keep existing dirty-bit behavior: scalar kind changes still mark parameter
  binds dirty so `mysql_stmt_bind_param()` rebuilds the bind array.
- When a scalar bind repeats with the same kind, update only that scalar field
  and `mysql_is_null`.
- Continue using `reset_to_null()` when changing kind so previous text/blob
  ownership and custom destructors are released.
- Do not change reset semantics: `mylite_reset()` keeps bindings, and
  `mylite_clear_bindings()` remains the explicit release point.

## Compatibility Impact

No SQL, storage-engine routing, file-format, or public API behavior changes.
The observable binding result is identical; only same-kind scalar binding work
is reduced.

## Single-File And Lifecycle Impact

No storage lifecycle or durable file impact.

## Binary-Size Impact

Negligible. The change adds small scalar assignment branches and no new
dependencies or public symbols.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Same-kind scalar rebinds no longer call `BoundValue::reset_to_null()`.
- Changing from text/blob/null to a scalar still releases previous bound
  resources and marks the MariaDB bind array dirty.
- Existing prepared statement reset, clear-bindings, scalar read, and routed
  storage update tests pass.
- The prepared-update benchmark completes with the expected checksum.

## Risks And Unresolved Questions

- The optimization relies on the existing invariant that same-kind scalar
  `MYSQL_BIND` entries point at stable `BoundValue` storage. This is already
  required by the dirty-bit reuse path.
- The largest remaining prepared-update cost is MariaDB planning and execution;
  this slice only removes a smaller libmylite API-loop cost.
