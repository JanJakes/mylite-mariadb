# Handler Active Checkpoint Hint

## Problem

MariaDB calls `ha_mylite::external_lock()` before routed row-DML execution. In
explicit transactions or `libmylite`-owned prepared row-DML checkpoints,
`external_lock()` already asks storage whether a checkpoint is active for the
primary `.mylite` file. The helper that decides whether to begin a handler
statement checkpoint immediately repeats the same active-chain lookup.

Prepared-update samples still show `mylite_storage_statement_active()` under the
handler lock path after libmylite has already opened the nested row-DML
checkpoint. That duplicate lookup does not change behavior, but it adds another
owner and filename walk to every routed prepared update.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::external_lock()` starts a
  transaction checkpoint when `OPTION_NOT_AUTOCOMMIT` or `OPTION_BEGIN` is set
  and no storage statement is already active.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_begin_statement_checkpoint()`
  begins a handler statement checkpoint only when no handler checkpoint exists
  and no storage statement is already active.
- `packages/mylite-storage/src/storage.c::mylite_storage_statement_active()`
  resolves activeness by walking the current owner-scoped active statement
  chain and comparing filenames.
- `libmylite` wraps direct and prepared row-DML inside the active storage
  context owner, so the active checkpoint observed in `external_lock()` is the
  same checkpoint that the helper would rediscover.

## Design

- Thread a boolean `storage_statement_known_active` hint from
  `external_lock()` into `mylite_begin_statement_checkpoint()`.
- Set the hint only when `external_lock()` has already observed an active
  storage checkpoint for the primary file in the current context owner.
- Keep the existing storage lookup when the hint is false. This preserves
  current behavior for the first handler-owned transaction checkpoint, volatile
  snapshots, unusual partially initialized contexts, and any future caller.
- Do not change the storage API or checkpoint ownership rules.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, or file-format behavior
changes. This removes a redundant internal guard lookup only after the same
guard has already succeeded in the caller.

## Single-File And Lifecycle Impact

No durable lifecycle changes. Handler statement checkpoints are still skipped
only while an active MyLite checkpoint owns the same primary file for the
current storage context, and otherwise start, commit, or roll back through the
existing paths.

## Binary-Size Impact

Negligible. The change adds one boolean parameter and no dependency.

## Tests And Verification

- Rebuild the MariaDB storage-smoke archive because the handler source changes:
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- Rebuild focused first-party targets:
  `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- Run:
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`

## Acceptance Criteria

- `external_lock()` does not ask storage twice when it has already proven a
  storage checkpoint is active.
- Handler-owned checkpoint behavior remains unchanged when no active storage
  checkpoint has already been observed.
- Routed transaction, savepoint, prepared row-DML, MEMORY/HEAP snapshot, and
  storage tests continue to pass.

## Risks And Unresolved Questions

- The optimization intentionally does not infer activeness after attempting to
  begin a transaction checkpoint. That keeps partially initialized volatile
  contexts conservative, but leaves one first-statement lookup in that path.
- MariaDB planning and execution still dominate prepared-row-DML timing; this is
  a small handler hot-path cleanup, not the planned navigable-index or
  SQL-execution bypass work.
