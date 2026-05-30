# Encoded Leaf Max Cell Read Site Counters

## Problem

After carrying leaf-range max fences forward, the prepared-insert benchmark
still reports `776` encoded index leaf max-cell reads. Source inspection points
at branch leaf-split child copying and direct branch snapshot encoding, but the
benchmark currently exposes only an aggregate count.

Before changing another writer path, MyLite needs caller-level attribution so
the next performance slice can remove redundant page-local max-cell reads
without touching planning, journal validation, checksum publication, or durable
read validation.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage test-hook observability and docs.
- `read_encoded_index_leaf_max_cell()` reads the last fixed-width leaf cell
  from already encoded leaf bytes. It does not validate the page checksum.
- Prepared-insert output after leaf-range fence carry-forward reports `776`
  aggregate encoded leaf max-cell reads.
- `copy_index_branch_children_with_split()` reads the max cell from the two
  freshly encoded split leaves while rebuilding branch child arrays.
- `encode_index_branch_page_from_leaf_run()` reads max cells from trusted
  encoded leaf runs while building direct branch snapshots.

## Design

Add test-hook-only call-site attribution for encoded leaf max-cell reads:

- wrap `read_encoded_index_leaf_max_cell()` with an at-site helper in
  test-hook builds;
- record caller function names in a bounded table parallel to the existing
  checksum, maintained-root decode, branch decode, and leaf encode tables;
- reset the table in prepared-insert profile reset;
- expose slot-count, slot-name, and count accessors; and
- print a prepared-insert benchmark table after the aggregate max-cell read
  counter.

The aggregate counter remains unchanged for older comparisons.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or transaction behavior
changes. Non-test-hook builds keep the same generated code shape except for
preprocessor-neutral declarations.

## Single-File And Lifecycle Impact

No durable files or transient companions are introduced. Journal protection,
rollback, recovery, commit, and close behavior stay unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. Test-hook builds add one bounded
caller-site table and three benchmark accessors. Production builds do not carry
the counters.

## Test And Verification Plan

- Extend focused branch refresh self-test coverage to assert fallback
  leaf-range refresh records the expected max-cell read call site, while the
  carried-fence refresh path records zero max-cell reads.
- Print encoded leaf max-cell read call sites in the prepared-insert benchmark.
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

- Benchmark output attributes the remaining encoded leaf max-cell reads by
  caller.
- Aggregate encoded leaf max-cell read counts remain available.
- No storage behavior, checksum timing, maintained-root validation, or journal
  validation changes.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `310.13 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,977,826` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `344.77 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  full-page checksum calls `8`, zero-tail checksum calls `235,291`, index
  leaf page clears `772`, and encoded index leaf max-cell reads `776`.
  The same sample reported `78.507 us/op`, but unrelated high-CPU Chrome load
  was active, so the timing is not treated as a clean comparison.
- Encoded leaf max-cell read attribution:

  | Site | Reads |
  | --- | ---: |
  | `encode_index_branch_page_from_leaf_run` | 4 |
  | `copy_index_branch_children_with_split` | 772 |

- Index-leaf encode attribution remains:

  | Site | Encoded leaf pages |
  | --- | ---: |
  | `prepare_index_branch_snapshot_pages_with_order` | 4 |
  | `prepare_zeroed_index_leaf_range_pages` | 24,796 |
  | `prepare_index_leaf_split_pages` | 772 |

- Maintained-root decode attribution remains `677` decodes:
  `read_index_leaf_run_root` `1` full-checksum decode,
  `plan_maintained_index_root_inserts` `674` decodes (`2` full checksum,
  `672` checksum-dirty), and `validate_recovery_journal_saved_page` `2`
  full-checksum decodes.
- Index-branch decode attribution remains `none | 0`.

## Risks

- Caller names are evidence, not API. Future refactors may change labels.
- This slice is attribution only; the follow-up behavior slice must still
  prove carried split or snapshot fences preserve branch validation.
