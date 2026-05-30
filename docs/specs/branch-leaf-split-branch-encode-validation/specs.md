# Branch Leaf Split Branch Encode Validation

## Problem

The prepared-insert branch decode-site counters attribute all remaining `386`
`index-branch` full-page decoder checksum calls to
`split_branch_index_leaf_entry`. Source inspection shows the hot decode is the
post-encode validation of the newly rebuilt level-one branch page, not a
durable read or recovery-journal validation pass.

That full decode recomputes a checksum and revalidates every encoded field for
bytes assembled from already decoded branch/leaf pages and freshly encoded
split leaves. The next safe performance slice is to replace that writer-local
decode with targeted validation of the branch components used to encode the
page.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `split_branch_index_leaf_entry()` reads the branch and selected leaf through
  cache-aware writer readers; those fallback paths still run durable checksum
  validation on cache misses.
- The function builds the replacement child arrays with
  `copy_index_branch_children_with_split()`, encodes the rebuilt level-one
  branch with `encode_index_branch_page()`, then decodes that same in-memory
  page solely for validation.
- The new branch page changes only the child array shape and fences for the
  split leaf; recovery-journal saved-page validation and durable future reads
  are separate gates and must keep their full decoders.

## Design

Add a small targeted validator for branch encoder inputs:

- require addressable page ids under the header that includes the new appended
  leaf page;
- require nonzero level/key/child count, child capacity, and entry-count bounds
  consistent with `decode_index_branch_page()`;
- reject child page ids that point at the branch page being encoded; and
- verify strict child fence ordering by key and row id.

Use that validator in `split_branch_index_leaf_entry()` before
`encode_index_branch_page()`, then remove the post-encode
`decode_index_branch_page()` call. Keep branch and leaf read validation,
journal validation, durable checksum writes, and page format unchanged.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Valid branch split writes produce the same
durable branch bytes.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The optimization affects
only transient writer-side validation after trusted in-memory branch split
assembly.

## Binary-Size And Dependency Impact

No dependency or license changes. Binary-size impact is limited to one small
first-party validator and a focused storage self-test.

## Test And Verification Plan

- Add a storage self-test proving the targeted validator accepts a future
  appended leaf page only when the updated header makes it addressable, rejects
  out-of-order child fences, rejects a child id equal to the branch page id, and
  does not record a branch decode site while checksum profiling is enabled.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- `split_branch_index_leaf_entry()` no longer calls
  `decode_index_branch_page()` on the branch page it just encoded.
- Prepared-insert branch decode-site output no longer reports
  `split_branch_index_leaf_entry`.
- Full-page checksum calls drop by the prior `386` branch decodes without
  reducing maintained-root planning or recovery-journal validation.
- Storage and storage-smoke verification pass.

## Implementation Evidence

Implemented with `validate_index_branch_encode_components()` and a focused
storage self-test that checks future appended leaf addressability, child-id
self-reference rejection, child-fence ordering rejection, zero full-page
checksum calls, one expected branch encode zero-tail checksum, and no branch
decode-site records while checksum profiling is enabled.

Verification passed:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The storage-smoke static archive measured `33,979,386` bytes (`32.41 MiB`).
The prepared-insert component benchmark reported `85.142 us/op`,
`1,943` full-page checksum calls, `235,291` zero-tail checksum calls, and
`0` `index-branch` full-page calls. Maintained-root decodes stayed at `1,449`,
split as `774` recovery-journal saved-page validation decodes, `674`
maintained-root planning decodes, and `1` root-to-leaf read conversion decode.
The branch decode-site table now reports `none | 0`.

## Risks

The validator covers the mutation surface rather than every byte of the just
encoded page. That is acceptable for this trusted writer path because
`encode_index_branch_page()` owns the byte layout, while the original branch
and leaf source pages were already decoded and recovery/durable read paths keep
full checksum-validating decoders.
