# Live Row-ID Cache Filename Identity

## Problem

After adding trusted handler filename identity scopes, prepared primary-key
update profiling still shows `_platform_strcmp` below
`find_durable_live_row_id_cache()`. The durable live-row-id cache owns copied
filenames, so its last-hit probe still falls back to content comparison even
when the handler has proved a stable primary filename pointer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_live_row_id_cache`
  stores an owned filename copy and validates entries by filename, catalog
  root/generation, page count where durable, and table id.
- The active and durable live-row-id cache sets already keep a last-hit index.
- The focused prepared-update sample after filename identity scopes showed the
  remaining MyLite-owned filename `strcmp()` under
  `load_active_live_row_id_cache_in_statement()` ->
  `find_durable_live_row_id_cache()`.
- Handler write locks, exact reads, and row updates now run inside a trusted
  filename identity scope for `mylite_primary_file_path()`.

## Design

- Add borrowed filename identity metadata to live-row-id cache entries beside
  the owned filename copy.
- Reuse the active filename identity scope to store borrowed identity only when
  the caller proves the current filename pointer is stable.
- Use the borrowed identity in active and durable live-row-id cache lookup,
  clear, and retarget probes before falling back to existing content
  comparison.
- Preserve the owned filename as the correctness source and the only memory
  that cache cleanup frees.

## Compatibility Impact

No SQL behavior, storage-engine routing, public API, or durable file-format
change. Cache hits still validate the same filename, catalog, page-count, and
table-id keys; the new identity is only a transient comparison shortcut.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle change. The borrowed filename
identity is transient in-memory state and does not affect journal or cache
invalidation semantics.

## Binary-Size And Dependency Impact

Small first-party storage-only change. No new dependency or build-profile
change.

## Tests And Verification

- Built `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Rebuilt `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` and
  relinked the embedded smoke binaries. The build still emits existing upstream
  missing-`override` and libtool no-symbol warnings.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran a focused prepared-update benchmark and sample; the sampled run measured
  2.389 us/op and no longer showed `_platform_strcmp` under
  `find_durable_live_row_id_cache()`.
- After unrelated `clang-tidy` load cleared, three focused prepared-update
  runs measured 2.366, 2.339, and 2.349 us/op.
- Passed `git diff --check` and `git clang-format --diff` on the touched C
  files.

## Acceptance Criteria

- Active and durable live-row-id cache filename probes can match by trusted
  borrowed filename identity.
- Raw storage callers without a filename identity scope keep the existing
  content-comparison behavior.
- Cache clear, retarget, last-hit, append, and free paths preserve existing
  invalidation and ownership behavior.
- Existing storage and embedded storage-engine tests pass.

## Risks And Open Questions

- The borrowed filename identity has the same stability contract as the
  statement filename identity scope. The handler primary filename satisfies it;
  raw callers with mutable filename buffers should not opt in.
