# Index Leaf Tail-Clear Encoding

## Problem

The current prepared-insert profile reports only protected maintained-root
decode sites, but still reports `772` index-leaf page clears under
`prepare_index_leaf_split_pages`. Those split pages are encoded through the
generic `encode_index_leaf_page()` wrapper, which clears all `4096` bytes before
writing metadata, cells, and the checksum.

For index-leaf pages, bytes before `used_bytes` are fully overwritten by the
encoder: the header occupies bytes `0..63`, and fixed-width cells occupy the
contiguous payload range starting at byte `64`. Reused or stack buffers only
need the unused tail zeroed before checksum calculation.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source is involved.
- `MYLITE_STORAGE_FORMAT_INDEX_LEAF_*` offsets define a contiguous leaf header
  through `MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET`.
- `encode_zeroed_index_leaf_page()` writes every header field and every cell
  byte up to `used_bytes`, then computes the normal zero-tail checksum.
- `encode_index_leaf_page()` currently protects reused buffers by full-page
  clearing before delegating to the zeroed-page body.
- Prepared-insert evidence after the wider-victim slice reports `772`
  `index leaf page clears`, all attributed to `prepare_index_leaf_split_pages`.

## Design

Change the generic `encode_index_leaf_page()` wrapper from full-page clearing to
tail-only clearing:

- zero bytes from `used_bytes` to the end of the page when the used prefix is
  within the page;
- retain the full-clear fallback for unexpected out-of-range used sizes;
- delegate to `encode_zeroed_index_leaf_page()` for header, payload, and
  checksum generation; and
- keep zeroed-buffer callers on `encode_zeroed_index_leaf_page()` unchanged.

The resulting page image should be byte-identical to encoding into a fully
zeroed page because all meaningful bytes are overwritten before the checksum is
calculated.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or durable bytes change.
The slice only reduces transient encoder memory clearing.

## Single-File And Lifecycle Impact

No journal, recovery, lock, sidecar, commit, rollback, or close behavior
changes. Durable pages remain checksum-valid before publication.

## Binary-Size And Dependency Impact

No new dependencies. The production change is a smaller memory clear in an
existing first-party encoder.

## Tests And Verification

- Extend focused storage self-tests so encoding into non-zero caller buffers:
  - produces byte-identical pages to encoding into a zeroed buffer;
  - leaves unused tails zeroed for direct and range encoding; and
  - no longer records full-page index-leaf clears.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Index-leaf pages encoded through the generic wrapper remain byte-identical to
  the previous full-clear contract.
- Prepared-insert `index leaf page clears` fall from `772` to `0`.
- Maintained-root decode sites remain protected and unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The safety of tail-only clearing depends on the leaf format remaining a
  fully overwritten contiguous header plus fixed-width cells. If the leaf format
  gains unwritten padding inside `used_bytes`, this wrapper must return to
  clearing that gap or the full page.

## Verification Evidence

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `303.60 sec`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `316.75 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,981,642` byte (`32.41 MiB`) `libmariadbd.a`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The prepared insert step sampled `77.708 us/op`, full-page checksum
  calls stayed at `8`, zero-tail checksum calls stayed at `227,063`,
  `index leaf page clears` fell from `772` to `0`, and encoded leaf max-cell
  reads stayed at `0`.

The maintained-root decode table stayed on protected validation sites:

- `read_index_leaf_run_root`: `1` decode, `1` full checksum;
- `plan_maintained_index_root_inserts`: `674` decodes, `2` full checksum,
  `672` checksum-dirty; and
- `validate_recovery_journal_saved_page`: `2` decodes, `2` full checksum.
