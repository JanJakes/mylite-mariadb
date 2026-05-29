# Dirty Page Buffer Pressure Admission Source Counters

## Problem

The prepared-insert pressure profile now reports which page family is admitted
after each buffer-limit flush, the incoming leaf occupancy, and the maintained
write site carried with that incoming page. A discarded local pager-admission
bypass experiment left those hot counters unchanged, which suggests that many
pressure admissions may be arriving through nested dirty-buffer merge rather
than the direct pager admission path.

Before changing pressure admission or direct-write policy, the benchmark needs
to distinguish direct dirty-buffer stores from child-statement dirty-buffer
merges.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `store_dirty_page_in_buffer()` records pressure incoming pages only when the
  dirty-page buffer is full and a buffer-limit flush makes a slot available.
- `merge_dirty_page_buffer()` replays each child dirty page into the parent
  dirty-page buffer. In test-hook builds it already preserves the original
  maintained pressure write-site name through
  `store_dirty_page_in_buffer_at_pressure_write_site()`.
- `record_dirty_page_buffer_pressure_incoming_page()` sees the incoming page
  family and checksum-dirty flag after pressure, but currently has no source
  dimension to say whether the admission was direct or came from dirty-buffer
  merge.

## Design

Add a test-hook-only pressure admission source counter:

- classify pressure admissions as `direct-store` by default;
- set the current admission source to `dirty-buffer-merge` while
  `merge_dirty_page_buffer()` replays child dirty pages into the parent;
- count incoming page family and checksum-dirty incoming page family by source;
- expose source slot-count, source name, source/family count, and
  source/family dirty count accessors; and
- print a prepared-insert benchmark table next to the existing pressure
  incoming and write-site tables.

The existing write-site propagation stays unchanged. Admission source answers
which path caused the buffer-limit admission; write site answers which
maintained writer originally produced the page.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
eviction order, direct-write fallback behavior, journal protection, rollback,
nested-statement isolation, page publication, checksum refresh timing, or
embedded open/close behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving:
  - a direct store into a full dirty-page buffer records `direct-store`; and
  - a child dirty-buffer merge into a full parent records
    `dirty-buffer-merge`.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

The VPS storage-smoke benchmark reported a `76.242 us/op` prepared insert step
with all buffer-limit pressure admissions attributed to dirty-buffer merge:

| Admission source | Page family | Incoming pages | Checksum-dirty incoming pages |
| --- | --- | ---: | ---: |
| dirty-buffer-merge | index-leaf | 85,257 | 85,257 |
| dirty-buffer-merge | index-branch | 275 | 140 |

No `direct-store` rows were reported. The source totals match the existing
`85,532` buffer-limit dirty-page pressure flushes, and explain why a local
pager-admission bypass experiment did not change the prepared-insert pressure
counters.

## Acceptance Criteria

- Prepared-insert benchmark output reports pressure admissions by source and
  page family.
- Existing pressure incoming, incoming leaf fill/free-slot, pressure
  write-site, flush, replacement, and checksum counters still report
  correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Admission-source counters identify where pressure enters the parent buffer;
  they do not prove a direct-write policy is safe. Any later bypass must still
  preserve rollback, nested-statement merge, journal protection, and active
  reader semantics with before/after benchmark evidence.
