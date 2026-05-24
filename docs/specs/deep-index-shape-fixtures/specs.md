# Deep Index Shape Fixtures

## Goal

Keep deep maintained-index regression coverage practical by adding storage-test
fixtures that synthesize sparse but valid branch shapes directly, then exercise
the normal public mutation path from that shape.

## Non-Goals

- No production storage behavior change.
- No new public MyLite API.
- No reduction in the behavioral assertions for level-`6` root promotion.
- No generated fixtures for every recursive split shape in this slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The relevant mutation still enters MyLite through MariaDB's handler row-write
  path and reaches first-party maintained-index planning in
  `packages/mylite-storage/src/storage.c`.
- `packages/mylite-storage/tests/storage_test.c` already has test-only branch
  and leaf page encoders, direct page writes, header page-count patching, and
  public index-root catalog publication. Those are enough to construct a
  branch tree shape without adding production-only hooks.

## Compatibility Impact

No SQL, C API, storage-engine routing, protocol, or file-format compatibility
change. This only changes how a storage regression reaches an existing
on-disk shape before calling the public append path.

## Design

Add a test fixture that creates a fresh `.mylite` file, stores the `app.posts`
table, writes a sparse level-`6` branch root whose rightmost path is packed to
the level-`6` root-promotion boundary, publishes that root through the existing
index-root catalog API, and then appends a high-key row through
`mylite_storage_append_row_with_index_entries()`.

The fixture writes real branch pages for every sibling page that the split code
reads while collecting entry counts and fences. It keeps unrelated sibling
subtrees sparse, but still writes valid child chains down to real full leaf
pages because production decoders require addressable branch children and
addressable leaf row ids. The rightmost selected leaf is real and full.

## File Lifecycle

The fixture still uses one primary `.mylite` file plus the existing statement
journal during rollback coverage. Direct page writes are test setup only. The
public append path must still journal-protect the mutated root, selected branch
path, and selected leaf, and rollback must restore the previous file size.

## Embedded Lifecycle And API

No embedded runtime or public API change.

## Build, Size, And Dependencies

No build-profile, binary-size, dependency, license, or fork-maintenance impact.

## Test Plan

- Replace the public-write level-`6` root fill in the packed-root split test
  with a fixture-backed level-`6` root-promotion regression.
- Keep rollback and commit assertions for root level, child counts, file size,
  exact lookup, indexed-row materialization for the inserted row, and prefix
  lookup.
- Run the focused storage test and static checks.

## Acceptance Criteria

- Level-`6` root-promotion coverage no longer needs to fill the entire root
  through public row appends.
- The public append path still performs the level-`7` promotion from the
  synthetic shape.
- Statement rollback restores the synthetic level-`6` root and file size.
- Focused storage-test runtime drops materially from the previous `~1000`
  second level-`6` promotion run.

## Verification

- `clang-format -i packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `163.36` seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure` passed in `192.23`
  seconds.

## Risks And Open Questions

- Sparse sibling subtrees are intentionally not full read-coverage fixtures.
  Keep public-write coverage for shallower split shapes until recursive split
  fixtures cover full traversal behavior.
- Future recursive split work should extend this fixture pattern rather than
  adding another deep public-fill loop.
