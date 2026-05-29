# Dirty Page Buffer Non-Max High-Fill Pressure

## Problem

Prepared-insert pressure still publishes `53,997` dirty index leaves before
statement commit. The rank/fill-band probe shows the largest targetable group
is older high-fill leaves, not max page-id leaves: `38,947` buffer-limit victims
are `non-max-leaf-page-id` in the `75-99%` band, while only `492` max page-id
victims are in that same band.

The current selector preserves branch ancestors, prefers clean leaves, and
prefers structurally full dirty leaves. When all candidate dirty leaves are
partial, it falls back to the first dirty leaf in round-robin order. That can
evict lower-fill non-max leaves before near-full non-max leaves that are less
likely to absorb many future inserts.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c` and
  `packages/mylite-storage/tests/storage_test.c`.
- `dirty_page_buffer_pressure_flush_index()` chooses the resident page flushed
  under `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT` pressure.
- `dirty_page_buffer_entry_index_leaf_fill()` already validates fixed-width
  leaf metadata and returns entry count plus capacity without decoding entries
  or refreshing checksums.
- The leaf page format does not encode sibling pointers, so max page id is only
  a current append-workload proxy for the right edge.

## Design

Refine dirty partial leaf pressure selection while preserving existing higher
priorities:

- keep clean leaf preference first;
- keep structurally full dirty leaf preference second;
- when choosing among dirty partial leaves, prefer the valid non-max page-id
  leaf with the highest `entry_count / entry_capacity` fill ratio;
- preserve round-robin order for equal fill ratios; and
- fall back to the existing first dirty leaf behavior when no valid non-max
  dirty partial leaf is available.

The max page-id exclusion keeps the heuristic from deliberately targeting the
current append-edge proxy. This does not claim max page id is a durable index
ordering invariant.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. This only changes
which buffered maintained-index page is flushed first under buffer-limit
pressure.

## Single-File And Lifecycle Impact

No files are introduced. Dirty pages still flush through the existing
journal-protected primary `.mylite` write path, and rollback, nested statement
merge, checksum-dirty handling, and statement commit behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Production builds add a bounded scan over the fixed-size
dirty-page buffer under pressure.

## Tests And Verification

- Add storage test-hook coverage proving dirty partial pressure chooses the
  highest-fill valid non-max leaf before a lower-fill non-max leaf.
- Add coverage proving a higher-fill max page-id leaf does not displace a valid
  lower-fill non-max candidate.
- Keep existing clean-leaf and full-leaf preference tests.
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

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `299.86 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `32.40 MiB` `libmariadbd.a`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `355.33 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed with a `81.696 us/op` prepared insert step.

The benchmark reported `85,532` buffer-limit index-leaf flushes. The selector
kept max page-id pressure low: `85,529` victims were `non-max-leaf-page-id`
and `3` were `max-leaf-page-id`, all three in the `full` band. Non-max
victims were concentrated in high-fill leaves: `6,727` in `50-74%`, `75,478`
in `75-99%`, and `3,324` in `full`.

## Acceptance Criteria

- Clean leaf and full dirty leaf preferences are preserved.
- Dirty partial pressure chooses the highest-fill valid non-max page-id leaf.
- A higher-fill max page-id leaf is not preferred over a valid lower-fill
  non-max page-id dirty partial leaf.
- Existing pressure, flush, rank, fill-band, matrix, replacement, and checksum
  counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Max page id is a workload proxy for append-edge behavior, not a semantic
  right-edge marker. If the prepared-insert benchmark regresses or pressure
  churn shifts toward max page-id victims, this slice should be reverted or
  redesigned around stronger index-edge evidence.
