# Maintained Root Decode Call Site Counters

## Problem

Prepared-insert checksum call-site output now shows that all `2,803`
`index-root` full-page checksum calls in the current storage-smoke profile come
from `decode_maintained_index_root_page()`. That narrows the hotspot to one
validator, but not to the storage caller that is repeatedly decoding maintained
single-page index roots.

Before changing checksum validation or root publication behavior, the benchmark
needs caller-level evidence for maintained-root decodes so a later optimization
can target the dominant path without weakening durable page validation.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook observability only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `decode_maintained_index_root_page()` validates maintained root metadata,
  the stored page id, supported flags, row-id references, sorted cells, and the
  full-page FNV checksum at
  `MYLITE_STORAGE_FORMAT_INDEX_ROOT_CHECKSUM_OFFSET`.
- Hot-path callers include maintained-root insert planning, packed-insert
  admission, root entry mutation, branch promotion/refill paths, read-side root
  conversion to leaf views, and storage test hooks.
- The checksum call-site counters cannot distinguish those callers because the
  full-page checksum is issued inside the shared decoder.

## Design

In test-hook builds, wrap `decode_maintained_index_root_page()` with a macro
that passes `__func__` into a site-aware helper implementation. Preserve the
existing function name and signature for non-test-hook builds.

Add a bounded maintained-root decode site table keyed by caller function name.
The table records calls that reach maintained-root checksum validation. The
benchmark prints sparse non-zero rows as:

`Site | Decodes`

The slice does not change checksum validation order, page parsing, mutation
behavior, dirty-buffer publication, rollback, or durable bytes.

## Implementation Notes

- Test-hook builds map `decode_maintained_index_root_page()` to a site-aware
  helper with `__func__`; non-test-hook builds keep the original static
  function name and signature.
- The decoder records caller attribution only when a page reaches maintained
  root checksum validation, keeping the new table aligned with the existing
  `index-root` full-page checksum count.
- The internal checksum call keeps the logical
  `decode_maintained_index_root_page` site label so the checksum call-site
  table remains comparable with earlier profiles.
- The prepared-insert counter reset clears the maintained-root decode site
  table and names.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or write policy changes.
The slice only adds test-hook counters and benchmark output.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, dirty-buffer pressure,
merge direct-write policy, statement commit, and embedded lifecycle behavior
remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add
a small bounded caller counter table and benchmark accessors.

## Tests And Verification

- Add a focused storage self-test that encodes and decodes a maintained
  index-root page through the public test hook, then asserts caller attribution
  and reset behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert profile reset clears maintained-root decode site counters.
- Benchmark output reports maintained-root decodes by caller function.
- Existing checksum counters, maintained-root validation behavior, rollback
  behavior, dirty-buffer publication, and storage routing remain unchanged.

## Verification Evidence

`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
on the VPS reported:

- prepared insert step: `76.945 us/op`;
- full-page checksum calls: `4,355`;
- zero-tail checksum calls: `243,497`;
- `2,803` `index-root` full-page checksum calls still appear under
  `decode_maintained_index_root_page`;
- maintained-root decode caller counts reconcile exactly to `2,803`:
  `774` `validate_recovery_journal_saved_page`, `674`
  `maintained_index_roots_allow_packed_insert`, `674`
  `plan_maintained_index_root_inserts`, `668`
  `insert_maintained_index_root_entry`, `6`
  `mark_maintained_index_root_overflow_tail`, `6`
  `promote_maintained_index_root_overflow_branch`, and `1`
  `read_index_leaf_run_root`.

## Risks

- Call-site names are function-level evidence, not stable public API. They are
  intended for local benchmark diagnosis and may change with refactors.
- The site table is bounded; if a future profile hits the limit, aggregate
  checksum counters still preserve the broader root-checksum signal and the
  limit can be increased in a separate evidence slice.
