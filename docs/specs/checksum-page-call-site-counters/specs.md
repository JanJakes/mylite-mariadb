# Checksum Page Call Site Counters

## Problem

Prepared-insert checksum output reports aggregate full-page and zero-tail
checksum calls by page family and phase. The current smoke profile shows
`4,355` full-page checksum calls, including `2,803` `index-root` calls in the
timed insert loop, but the benchmark does not identify which storage functions
issue those calls.

Without caller-level evidence, follow-up checksum work can see the page family
that dominates the profile but cannot distinguish root publication, validation,
journal, catalog, or other call paths before changing behavior.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook observability only:
  `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`. The existing storage test driver continues
  to call the focused checksum self-test.
- `checksum_page()` records aggregate full-page checksum calls and page-family
  counts in test-hook builds.
- `checksum_page_zero_tail()` records aggregate zero-tail checksum calls and
  page-family counts in test-hook builds.
- Existing benchmark output already snapshots checksum counters by phase, so a
  call-site table can be added without changing the phase snapshot structure.

## Design

In test-hook builds, wrap `checksum_page()` and `checksum_page_zero_tail()` with
macros that pass `__func__` into site-aware helper implementations. Preserve
the existing function names and signatures for non-test-hook builds.

Add a bounded test-hook site table keyed by function name and page family:

- full-page checksum calls;
- zero-tail checksum calls.

The benchmark prints sparse non-zero rows as:

`Site | Page family | Full-page | Zero-tail`

The existing aggregate checksum counters, phase snapshots, and page-family
tables remain unchanged for continuity with prior profiles.

## Implementation Notes

- Test-hook builds map `checksum_page()` and `checksum_page_zero_tail()` to
  site-aware helpers with `__func__`; non-test-hook builds keep the original
  static function names and signatures.
- The prepared-insert counter reset clears the site table, names, full-page
  counts, and zero-tail counts.
- Accessors return bounded slot counts, caller names, and per-family full-page
  or zero-tail counts, returning `NULL` or `0` for out-of-range slots.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, checksum algorithms, or write
policy changes. The slice only adds test-hook counters and local benchmark
output.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, dirty-buffer pressure,
merge direct-write policy, statement commit, and embedded lifecycle behavior
remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add
a small bounded site counter table and benchmark accessors.

## Tests And Verification

- Extend the checksum page-family self-test to assert that full-page and
  zero-tail checksum calls are attributed to the current storage test function
  and page family.
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

- Prepared-insert profile reset clears checksum call-site counters.
- Benchmark output reports checksum calls by caller function and page family.
- Existing aggregate checksum counters, checksum validation behavior,
  dirty-refresh counters, rollback behavior, and storage routing remain
  unchanged.

## Verification Evidence

`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
on the VPS reported:

- prepared insert step: `76.430 us/op`;
- full-page checksum calls: `4,355`;
- zero-tail checksum calls: `243,497`;
- all `2,803` `index-root` full-page checksum calls came from
  `decode_maintained_index_root_page`;
- full-page `header` calls split as `389` under `decode_header_page`;
- full-page `catalog` calls split as `389` under
  `validate_catalog_page_bytes`;
- full-page `journal` calls split as `387` under
  `decode_recovery_journal_header` and `1` under
  `encode_recovery_journal_header`;
- verification contributed `107,078` zero-tail `row` calls through
  `decode_row_page_metadata`.

## Risks

- Call-site names are function-level evidence, not stable public API. They are
  intended for local benchmark diagnosis and may change with refactors.
- The site table is bounded; if a future profile hits the limit, the benchmark
  still preserves aggregate counters and can grow the test-hook limit in a
  separate evidence slice.
