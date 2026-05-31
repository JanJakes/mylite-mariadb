# Merge Guard Single-Pass Leaf Fill

## Problem

The prepared-insert profile still reports `122,388` future-current dirty
`index-leaf` merge guard rows. For each eligible incoming leaf, the guard checks
the same leaf fill metadata multiple times while deciding between full,
near-full, `16-31` free-slot direct write, and the broader pressure-victim
policy. Partial fallback leaves can parse the same entry count, used bytes, and
capacity four times before the unchanged policy decision is made.

This is production hot-path work, not test-hook-only accounting. The next safe
slice is to classify the incoming leaf free-slot count once per merge entry and
reuse that classification across all merge guard predicates.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source changes are needed.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  validates the future-current boundary, append-buffer absence, leaf page type,
  and parent-resident checks before direct-write policy decisions.
- The helper then calls separate fill/free-slot helpers for full leaves,
  near-full leaves, `16-31` free-slot leaves, and broad victim policy input.
- `dirty_page_buffer_merge_broad_victim_guard_outcome()` calls
  `init_dirty_page_buffer_merge_broad_victim_guard_context()`, which recomputes
  incoming free slots before using the same pressure context and victim facts.
- Existing focused storage self-tests already cover the direct-write guard
  outcomes for protected existing pages, full future-current pages, near-full
  pages, `16-31` pages, replaced-broad victims, dense/equal/wider victims, and
  partial fallback with a planned pressure victim.

## Design

Add a small incoming leaf fill classification helper that returns the
free-slot count, including `0` for full leaves, after the common future-current
leaf and parent-resident checks have passed. Use that one result to choose:

- full leaf direct write (`0` free slots);
- near-full direct write (`1-15` free slots);
- `16-31` free-slot direct write; and
- broad victim policy for `32-127` free-slot leaves.

Pass the already-computed incoming free-slot count into the broad victim guard
context. Keep pressure-context selection, victim inspection, direct-write guard
outcome names, direct-write predicates, fallback replay, planned pressure
stores, checksum publication, and rollback behavior unchanged.

If incoming fill metadata cannot be classified, preserve the current safe
fallback behavior by returning `future-current-header-partial-leaf`.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, transaction semantics, or error-surface changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded
lifecycle change. The refactor only reuses transient metadata derived from the
same child dirty-buffer page.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Binary-size
impact is expected to be negligible and is checked by the static MariaDB smoke
build.

## Tests And Verification Plan

- Reuse existing focused storage self-tests for direct-write guard outcomes and
  planned fallback replay.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Existing guard outcome self-tests pass without expectation changes.
- Prepared-insert guard outcome counts remain structurally equivalent:
  `66,144` dirty leaf merge direct writes, `21,031` dirty leaf pressure
  admissions, and `21,031` `future-current-header-partial-leaf` fallback rows
  stay explainable.
- Checksum counters do not regress: `8` full-page checksum calls, `227,063`
  zero-tail checksum calls, `87,176` dirty `index-leaf` refreshes, and `677`
  maintained-root decodes remain the comparison baseline.
- The sampled prepared-insert step does not regress materially.

## Risks

- An invalid incoming leaf must still choose the conservative fallback outcome.
- Broad victim policy must not lose the existing pressure-context plan; fallback
  replay still depends on reusing that selected victim safely.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `318.04 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `316.56 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,983,226` bytes and `478` archive members.

The final storage-smoke prepared-insert profile
(`/tmp/mylite-merge-guard-single-pass-leaf-fill-benchmark-3.txt`) reported:

- prepared insert step: `70.928 us/op`;
- future-current header leaf guard rows: `122,388`;
- dirty leaf merge direct writes: `66,144`;
- dirty leaf pressure admissions:
  `21,031` `future-current-header-partial-leaf` rows;
- dirty `index-leaf` refreshes: `87,176`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`; and
- merge pressure context builds/planned stores: `31,938` / `19,053`.

The guard outcome split was structurally unchanged:

- `3,827` full future-current direct writes;
- `31,312` near-full direct writes;
- `18,120` `16-31` free-slot direct writes;
- `4,218` replaced-broad-victim direct writes;
- `2,343` dense-broad-victim direct writes;
- `2,147` equal-broad-victim direct writes;
- `2,058` equal-dense-victim direct writes;
- `2,119` wider-victim direct writes; and
- `21,031` partial-leaf fallback rows.

This slice is not a dirty checksum publication change. Its performance value is
that production guard policy now parses the incoming leaf fill metadata once
for the future-current leaf decision path instead of repeating the same parse
across full, near-full, `16-31`, and broad-victim predicates. The local timing
sample is recorded as machine-dependent smoke evidence; the stable acceptance
evidence is unchanged guard, checksum, and decode counters plus the single-pass
source shape.
