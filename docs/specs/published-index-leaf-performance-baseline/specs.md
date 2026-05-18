# Published Index Leaf Performance Baseline

## Problem

MyLite can now publish single-level index leaf roots after explicit fixed-width
`CREATE INDEX` and `ALTER TABLE ... ADD KEY` copy rebuilds. The local
performance baseline still reports secondary-index exact select timings without
separating append-log scan fallback from a published leaf-root path, so future
storage work lacks a direct before/after signal for the optimization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c` creates `perf_rows` with
  `KEY value_key (value)`, inserts rows, and measures direct/prepared exact
  reads through `FORCE INDEX (value_key)`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rename_table()` is now the
  SQL lifecycle point that publishes supported index leaf roots after explicit
  copy-rebuild index DDL finalizes the durable table identity.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_current_command_publishes_index_leaf_roots()`
  intentionally publishes only explicit `CREATE INDEX` and
  `ALTER TABLE ... ADD KEY` roots, not initial `CREATE TABLE` index metadata.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  keeps publication opportunistic: if the leaf cannot fit in one page or the
  catalog lacks reserved headroom, exact reads remain correct through the
  append-log scan fallback.
- `docs/specs/performance-baseline-harness/specs.md` defines the benchmark as
  local machine-dependent evidence, not a CI threshold or competitive product
  claim.

## Design

- Keep the existing `value_key` secondary exact select rows as the scan-fallback
  comparison path.
- After the existing direct and prepared `value_key` measurements, populate a
  separate `perf_leaf_rows` table with the same deterministic rows and run
  `CREATE INDEX value_leaf_key ON perf_leaf_rows (value) ALGORITHM=COPY`. This
  keeps the existing update benchmark on `perf_rows` from silently maintaining
  an extra secondary index while still measuring the explicit index-publish
  path.
- Confirm publication through `mylite_storage_read_index_root()` before
  labelling any timing rows as published-leaf timings.
- Add direct and prepared exact select rows for
  `FORCE INDEX (value_leaf_key)` only when a root was actually published.
- Preserve checksum and returned-row validation for all measured paths.
- Keep larger configurable benchmark runs robust: if a larger row count cannot
  publish a single-page leaf, print a skip note instead of failing the whole
  benchmark or mislabelling a scan-fallback read as leaf-backed.

## Compatibility Impact

No SQL or C API behavior changes. The benchmark exercises ordinary routed
`ENGINE=InnoDB` table DDL and equality predicates through public `libmylite`.

## Single-File And Lifecycle Impact

No storage format change. The benchmark still creates one temporary `.mylite`
file plus MyLite-owned runtime companions under the temporary benchmark root
and removes them on exit.

## Public API And File-Format Impact

No public API or file-format change. The benchmark reads existing first-party
storage metadata to verify that a leaf root exists before measuring the
leaf-backed path.

## Storage-Engine Routing Impact

The benchmark continues to route `ENGINE=InnoDB` to MyLite storage. This keeps
the measurement aligned with application schemas that request InnoDB while
MyLite owns durable storage.

## Binary-Size And Dependency Impact

No production dependency or default library-size change. The benchmark already
links the storage library in the opt-in storage-smoke profile.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run `tools/mylite-perf-baseline` with default rows and iterations.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1` to verify
  oversized single-page leaves skip leaf-labelled timing rows.
- Run the storage-engine compatibility harness to keep SQL index DDL coverage
  honest after changing benchmark setup flow.
- Run changed-line formatting checks and `git diff --check`.

## Local Verification

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `tools/mylite-perf-baseline`
  - Default local run measured scan-fallback direct/prepared secondary exact
    selects at `4566.060` / `4704.440` us/op.
  - The same run prepared a separate leaf benchmark table, published a
    `value_leaf_key` root with `100` entries, and
    measured direct/prepared published-leaf secondary exact selects at
    `8446.540` / `8328.510` us/op.
  - These are noisy workstation numbers and not product claims.
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Verified the benchmark skips leaf-labelled rows when `1000` rows exceed the
    current single-page leaf limit.

## Acceptance Criteria

- Default benchmark output includes direct and prepared published-leaf secondary
  exact select rows, with checksums and row counts.
- Larger benchmark runs that cannot publish a single-page leaf skip the
  leaf-labelled rows instead of making false performance claims.
- Existing primary-key, secondary scan-fallback, update, and ordered-scan
  benchmark rows continue to run and validate their checksums.
- Documentation continues to describe benchmark results as local evidence only.

## Risks

- The comparison still includes MariaDB embedded SQL-layer overhead and is not a
  storage-only microbenchmark.
- The currently published leaf is a single page plus append-tail overlay, not a
  maintained multi-page B-tree. Larger row counts can skip the leaf path until
  multi-page index roots exist.
- Results are expected to remain noisy on a developer workstation.
