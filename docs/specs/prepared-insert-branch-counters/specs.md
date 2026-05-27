# Prepared Insert Branch Counters

## Problem

Prepared-insert timing has become branch-maintenance dominated enough that
wall-clock component timings alone do not clearly identify the next safe
storage change. A recent attempt to bound large branch refolds by falling back
to append-tail overlays was rejected because larger runs showed worse behavior,
so the benchmark needs durable per-run evidence about which branch maintenance
paths actually execute.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage and harness work only.
- `tools/mylite_perf_baseline.c::benchmark_prepared_insert_components()` is the
  focused prepared-insert component benchmark used for branch-maintenance
  performance work.
- `packages/mylite-storage/src/storage.c` already exposes test-hook counters
  for branch leaf-range planning reads, refold root reads, refold entryset
  reads, refold entryset cache hits, level-two branch-leaf planning reads,
  active branch-page planning reads, insert-writer branch/leaf decodes, and
  branch-tail overlay scans.
- `packages/mylite-storage/CMakeLists.txt` enables
  `MYLITE_STORAGE_TEST_HOOKS` for first-party testing builds, which is also
  the profile used by the local storage-smoke performance harness.

## Design

- Compile `mylite_perf_baseline` with `MYLITE_STORAGE_TEST_HOOKS` when
  `BUILD_TESTING` is enabled, matching the storage library's test-hook profile.
- Reset branch-maintenance counters immediately after the prepared insert
  statement is created and before the measured row loop starts.
- After the benchmark verifies the final row count, print a small Markdown
  table with the branch-maintenance counters.
- Keep non-testing builds independent of the hook symbols by wrapping both
  declarations and calls in `MYLITE_STORAGE_TEST_HOOKS`.

## Compatibility Impact

No SQL, public C API, storage-engine routing, durable storage, or file-format
behavior changes. The slice only changes local benchmark diagnostics in the
testing profile.

## Single-File And Lifecycle Impact

No companion files, locks, journals, recovery paths, or storage lifecycle
changes. Benchmark counters are thread-local diagnostic state and do not touch
the `.mylite` file.

## Binary Size And Dependencies

No new dependencies. The default non-testing storage library is unchanged; the
storage-smoke benchmark binary gains only small diagnostic calls and output.

## Tests And Verification

- Build the storage-smoke benchmark target.
- Run the prepared-insert component benchmark and verify the counter table is
  emitted after successful row-count validation.
- Run `git diff --check`, clang-format diff checks for touched C files, the
  storage unit CTest selection, and the embedded storage-engine smoke CTest.

Verification results:

- `git diff --check`
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  emitted the counter table; the observed run reported `65` branch-tail overlay
  scans and `1407` branch-tail overlay scan reads, with zero branch refold
  entryset reads.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`

## Acceptance Criteria

- `prepared-insert-components` reports branch-maintenance counters in the
  storage-smoke test-hook profile.
- Non-testing builds do not require test-hook symbols from `mylite_storage`.
- Existing storage and embedded storage-engine tests pass.
- The roadmap records that prepared-insert benchmark runs now include branch
  maintenance counters for follow-up performance decisions.

## Risks

The counters are diagnostic evidence, not a performance improvement by
themselves. The next implementation slice should use these numbers with
profiling samples before changing branch-refold or overlay selection again.
