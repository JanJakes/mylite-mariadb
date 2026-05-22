# Active Indexed Row Statement Header Fast Path

## Problem

After active direct-update row reads switched to
`mylite_storage_find_indexed_row_in_statement_into()`, delayed samples no longer
show the generic file open/close wrapper. They still show
`find_indexed_row_payload_in_scope()` and occasional
`read_header_from_file_scope()` frames under the active statement lookup.

That path is still slightly too generic for callers that already pass a
borrowed `mylite_storage_statement *`: the statement already owns the file,
current header, filename, and cache chain.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_find_indexed_row_in_statement_into()`
  currently builds a temporary `mylite_storage_file_scope` and calls
  `find_indexed_row_payload_in_scope()`.
- `find_indexed_row_payload_in_scope()` then resolves the `FILE *`, active
  cache statement, active mutation statement, and header from the file scope.
- For active statement callers, `statement->file`, `statement->filename`,
  `statement->current_header`, `statement->header`, and `statement->parent`
  already carry that state.
- The filename-based `mylite_storage_find_indexed_row_into()` still needs the
  generic file-scope helper because it must recover, lock, borrow or open the
  file, and close it correctly for raw callers.

## Design

- Add an internal `find_indexed_row_payload_in_statement()` helper for borrowed
  active statements.
- Add a shared lower helper that receives `FILE *`, filename, header,
  cache-statement owner, mutation statement, and table/key inputs.
- Keep `find_indexed_row_payload_in_scope()` for filename-based callers, but
  let it delegate to the shared lower helper after it reads the header from the
  scope.
- Let the active statement helper read the header directly from
  `statement->current_header` when present, falling back to `statement->header`.
- Resolve the active cache statement from the borrowed statement chain directly
  instead of rediscovering ownership through a file scope. The borrowed pointer
  is only valid for callers that already own or have borrowed that active
  statement.

## Affected Subsystems

- MyLite storage indexed-row lookup internals.
- MyLite storage public C API implementation for the scoped active lookup.

## Compatibility Impact

No SQL result, warning, error, affected-row, or storage result behavior should
change. The same exact-index lookup, table-entry cache, payload read, and
live-row validation helpers remain in use.

## DDL Metadata Routing Impact

No DDL metadata routing change. The active path still reads through the current
statement catalog/header view.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
Raw callers keep the filename-based recovery and locking path.

## Public API And File-Format Impact

No new API beyond the active scoped lookup added by the previous slice. The
`.mylite` file format is unchanged.

## Storage-Engine Routing Impact

No routing-policy change.

## Binary-Size Impact

Expected to be small noise: the lookup body is refactored into a shared helper,
with one active-statement wrapper.

## License Or Dependency Impact

None.

## Tests And Verification

- `git diff --check` passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c` passed
  with no formatting diff.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed `2/2`.
- `ctest --preset storage-smoke-dev --output-on-failure` passed `10/10`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` passed with
  archive `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `21,186,576` bytes (`20.21 MiB`).
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000` reported prepared primary
  key update components at bind `0.023 us/op`, step `2.104 us/op`, and reset
  `0.023 us/op` in an unsampled repeat.
- A delayed one-second sample of that benchmark showed the accepted
  direct-update target-row path under
  `mylite_storage_find_indexed_row_in_statement_into()`,
  `find_indexed_row_payload_with_header()`,
  `find_exact_index_row_id()`, and
  `read_indexed_row_payload_from_open_file()`. It did not show
  `find_indexed_row_payload_in_statement()`,
  `find_indexed_row_payload_in_scope()`, or `read_header_from_file_scope()` on
  that path.

## Acceptance Criteria

- Active statement indexed-row lookups no longer call
  `find_indexed_row_payload_in_scope()` or `read_header_from_file_scope()` on
  the accepted direct-update target-row path.
- Filename-based indexed-row callers still use the file-scope helper.
- Existing storage and embedded storage-engine tests pass.

## Risks And Unresolved Questions

- The active helper must not trust arbitrary stale statement pointers. The
  public scoped API continues to require a borrowed active statement pointer
  from the current active scope; its misuse check guards null statement, file,
  and filename values, but it cannot prove an arbitrary stale pointer is live.
