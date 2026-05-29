# Dirty-Page Buffer Merge Guard Counter Heap Storage

## Problem

The dirty-page buffer merge benchmark now exposes many high-dimensional
test-hook counters keyed by merge direct-write guard outcome. A rejected
below-tail direct-write experiment showed that adding one more guard outcome
expanded existing static thread-local counter tensors enough for the embedded
MariaDB smoke benchmark to abort with `Can't initialize timers`.

The next roadmap slice should remove that static TLS pressure without changing
storage behavior, so future evidence slices can add guard dimensions or
outcomes without threatening embedded startup.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB handler or SQL
  source is changed.
- `merge_dirty_page_buffer()` classifies every child dirty-buffer entry with
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` and records
  guard outcome counters before either direct-writing or replaying the page
  into the parent dirty buffer.
- The static TLS arrays currently keyed by
  `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_MERGE_DIRECT_WRITE_GUARD_COUNT` include:
  guard outcome family counters, guard outcome leaf fill/free-slot counters,
  fallback replacement counters, fallback parent-rank admission/replacement
  counters, fallback flush replacement-state counters, and fallback
  parent-rank flush replacement-state counters.
- Tail-distance counters already use lazy heap storage behind a thread-local
  pointer. That pattern avoided the embedded startup failure for the
  tail-distance slice and should be generalized to the rest of the guard keyed
  tensors.

## Design

Introduce a test-hook-only lazy heap allocation for merge guard/fallback
counter tensors. Keep one `_Thread_local` pointer as the only new TLS state and
move these existing arrays into the heap-backed struct:

- direct-write guard outcome family and dirty-family counts;
- direct-write guard outcome leaf fill/free-slot/free-slot-detail counts;
- fallback leaf replacement counts;
- fallback parent-rank admission and replacement counts;
- fallback leaf flush replacement-state counts;
- fallback parent-rank flush replacement-state counts;
- the existing fallback tail-distance admission/replacement/flush-state
  counts.

Recording functions allocate the struct on first use and skip only the new
heap-backed counter write if allocation fails. Accessors return zero when the
struct is not allocated or a slot is out of range. Reset allocates and zeroes
the struct when possible, matching the existing tail-distance behavior.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. Non-test-hook storage builds do not carry these counters.

## Single-File And Lifecycle Impact

No files or durable state are introduced. The change affects only process-local
test-hook benchmark counters.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds trade static TLS
for lazy heap storage. This should preserve embedded startup headroom while
keeping benchmark output stable.

## Tests And Verification

- Keep existing storage self-tests that assert guard outcome, fallback
  replacement, parent-rank, and tail-distance counters.
- Verify the embedded storage-engine smoke test can initialize after the static
  TLS reduction.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `302.86 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes and no `Can't initialize timers` abort;
  - storage-smoke CTests passed, including `mylite-storage` in `302.65 sec`
    and `libmylite.embedded-storage-engine` in `18.73 sec`;
  - prepared-insert benchmark reported `76.765 us/op` for the step component,
    `53,136` dirty leaf direct merge writes, `34,484` dirty leaf pressure
    admissions, and populated guard/fallback output tables.
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

- No static `_Thread_local` guard-outcome counter tensors remain for dirty-page
  buffer merge/fallback reporting.
- Existing benchmark output and focused storage counter tests still report the
  same counter families.
- Storage-smoke embedded startup passes, proving the TLS pressure reduction did
  not regress the MariaDB embedded timer path.
- No direct-write, dirty-buffer replacement, rollback, journaling, or file
  format behavior changes.

## Risks

- Allocation failure suppresses test-hook counter updates for heap-backed
  tensors. Storage behavior must continue unchanged.
- Reset now allocates a heap counter block in test-hook runs; that is acceptable
  for benchmark/test processes but should not leak into non-test-hook builds.
