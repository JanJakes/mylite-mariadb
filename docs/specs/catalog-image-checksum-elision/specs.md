# Catalog Image Checksum Elision

## Problem

The checksum page-family counter slice showed that the prepared-insert
component phase still executes one catalog zero-tail checksum per prepared
insert step:

- `catalog` zero-tail checksum calls: `100,001`
- prepared insert step: `83.564 us/op`

The visible call source is `initialize_catalog_image()`, which initializes a
transient in-memory catalog-image buffer by calling the durable empty catalog
page initializer. That durable initializer correctly computes a catalog page
checksum, but catalog-image buffers are record containers: later append paths
mutate their header fields without refreshing the checksum, and durable
publication re-encodes real catalog pages with fresh checksums.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party catalog image loading lives in
  `packages/mylite-storage/src/storage.c::read_catalog_image()` and
  `read_catalog_image_from_root_page()`.
- `initialize_catalog_image()` allocates a transient image and currently calls
  `initialize_empty_catalog_page()`.
- `append_catalog_image_records_from_page()` and catalog record append helpers
  update the in-memory image's record count and used bytes without recomputing
  the image checksum.
- `write_catalog_chain_pages()` initializes durable catalog pages and calls
  `update_catalog_checksum()` after laying out each published page.

## Design

Split empty catalog page initialization into two internal helpers:

- a header initializer that writes the catalog page magic, page type, version,
  checksum algorithm, root page id, empty generation, and used bytes without
  computing a checksum; and
- the existing durable empty-page initializer, which calls the header helper
  and then refreshes the checksum.

Use the header-only helper from `initialize_catalog_image()`. Durable empty
database initialization and catalog publication keep using
`initialize_empty_catalog_page()` and still write valid checksums.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. Durable catalog pages continue to be checksummed before they are
written to the `.mylite` file.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to a small internal helper split. No dependency or license change.

## Test And Verification Plan

- Add storage test-hook coverage proving catalog-image initialization does not
  call checksum routines, while durable empty catalog page initialization still
  does.
- Run the prepared-insert benchmark and confirm catalog zero-tail checksum
  calls fall from `100,001`.
- Keep storage and routed embedded storage-engine tests passing.
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

- Catalog-image initialization avoids checksum work.
- Durable empty catalog page initialization still refreshes a valid checksum.
- Prepared-insert benchmark output shows catalog zero-tail checksum calls
  removed or reduced to non-hot setup noise.
- Existing storage and embedded storage-engine tests pass.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `325.70 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `361.00 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `73.533 us/op`. Total zero-tail
  checksum calls dropped to `284,426`, and catalog zero-tail checksum calls
  dropped from the prior `100,001` to `0`.

## Risks

The slice depends on catalog images remaining transient record containers. If a
future path writes `catalog->bytes` directly as a durable catalog page, that
path must refresh the checksum first or use the durable page initializer.
