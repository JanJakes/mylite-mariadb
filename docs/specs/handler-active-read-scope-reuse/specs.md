# Handler Active Read-Scope Reuse

## Problem

`libmylite` can now retain one connection-owned storage read scope across fully
drained prepared read reset/re-execute loops, reducing the repeated
open/recovery/shared-lock/header setup cost. Handler-local
`Mylite_read_statement_scope` instances still call
`mylite_storage_begin_read_statement()` for each cursor construction. When an
outer read scope is already active for the same storage context and primary
file, that call creates and closes a nested read statement that only clones
already validated state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` wraps handler cursor and catalog reads
  in `Mylite_read_statement_scope`.
- `packages/mylite-storage/src/storage.c` keeps read statements in a
  thread-local stack and resolves current-context parents with
  `active_read_statement_for(filename)`.
- `open_existing_file_scope()` already reuses the active read statement for the
  current context and filename, so storage calls inside a handler operation do
  not require a nested child statement when an outer read scope is present.
- `mylite_storage_context_has_active_statement()` already exposes the
  current-context write statement state to callers, but there is no equivalent
  first-party helper for active read statements.

## Design

Add a small storage helper:

```c
int mylite_storage_context_has_active_read_statement(const char *filename);
```

The helper returns true only when the current storage context owner has an
active read statement for the given primary file. It does not inspect other
owners and does not change read-statement lifetime.

Update `Mylite_read_statement_scope` so it remains narrow but avoids opening a
nested read statement when either:

- the current context already has an active write statement, matching the
  existing `mylite_storage_begin_read_statement()` no-op behavior, or
- the current context already has an active read statement for the primary
  file.

Storage calls inside the handler then use `open_existing_file_scope()` to borrow
the active statement directly.

## Non-Goals

- This slice does not broaden handler read-lock lifetime.
- This slice does not change prepared statement API behavior.
- This slice does not implement WAL/MVCC read snapshots.
- This slice does not alter storage recovery, locking, or checkpoint rules.

## Compatibility Impact

No SQL or public API compatibility change is intended. The optimization is
internal to the MyLite handler/storage boundary and preserves the explicit
active-result write rejection added by the prepared read-scope reuse slice.

## Single-File And Lifecycle Impact

No new files or companions are introduced. Active read statements keep the same
primary `.mylite` file lifetime and shared-lock behavior as before.

## Public API And File-Format Impact

No `libmylite` public API or file-format change is required. The storage helper
is a first-party internal API used by the MyLite handler and tests.

## Storage-Engine Routing Impact

The change applies only to routed MyLite handler reads against durable primary
files. Volatile and row-discard paths keep their existing behavior.

## Binary-Size Impact

Expected impact is a small helper and one branch in the handler read-scope
constructor.

## Test And Verification Plan

- Add storage unit coverage for current-context active read detection, including
  same-file true and no-active-read false cases.
- Run storage unit tests and storage-enabled embedded statement tests.
- Run targeted routed prepared primary-key performance before committing.
- Run compatibility groups covering prepared statements, storage-engine routing,
  locking, and transactions if the implementation changes handler behavior.

## Acceptance Criteria

- Handler-local read scopes skip nested begin/end work when an outer current
  context read scope already covers the same primary file.
- Existing read/write statement semantics remain unchanged.
- Tests cover the new storage helper and the prepared read path still passes.
- Local prepared primary-key point-select performance is not regressed.

## Verification

Implemented in this slice and verified locally with:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage\.capabilities|libmylite\.embedded-statement' --output-on-failure`
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite\.embedded-storage-engine|libmylite\.embedded-handler-savepoint' --output-on-failure`
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`

The local routed prepared primary-key point-select sample measured
2.263 us/op after the handler stopped creating nested read statements when the
current storage context already had an outer read scope.

## Risks And Unresolved Questions

- The helper must stay current-context-only. Reusing a read statement owned by a
  different `mylite_db` would break handle isolation and lock ownership.
- This is an incremental hot-path reduction. The larger remaining gap to
  SQLite-like reads still needs broader pager/index and concurrency work.
