# Storage Exact Index Entry Lookup

## Problem

The handler can now build filtered cursors for exact-key reads, but the durable
and volatile storage APIs still materialize every live index entry before the
handler filters the set. For primary-key-style point reads this spends work on
keys and row ids that the caller will immediately discard.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` starts equality ranges
  through `ha_index_read_map()`, and `handler::read_range_next()` uses
  `ha_index_next_same()` for the matching range.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  scans append-only index-entry pages, filters table id, index number, and
  row-state tombstones, then appends every live entry to an allocated entryset.
- `packages/mylite-storage/src/storage.c::mylite_storage_index_prefix_exists()`
  already proves a scan can stop at the first matching key prefix without
  building an entryset.
- `mariadb/storage/mylite/mylite_volatile_rows.cc::mylite_volatile_read_index_entries()`
  performs the same allocation-heavy live-entry materialization for
  runtime-volatile MEMORY/HEAP tables.

## Design

- Add durable and volatile storage lookup APIs that scan live entries for one
  exact key image and return the matching row id without allocating a full
  `mylite_storage_index_entryset`.
- Keep the lookup byte-exact and use it only from the handler for the same
  guarded raw exact path: non-nullable full-key unique integer key images.
  Collation-sensitive string and general prefix filters keep using the handler
  key-tuple comparison path from full entrysets.
- Populate a one-entry handler cursor from the returned row id and copied key
  bytes, so `index_next_same()` and cursor cleanup keep the same lifecycle as
  other index reads.
- Return `MYLITE_STORAGE_NOTFOUND` for a valid lookup with no matching live
  entry. Preserve `MYLITE_STORAGE_MISUSE`, `MYLITE_STORAGE_NOMEM`,
  `MYLITE_STORAGE_FULL`, and I/O error behavior for invalid inputs and storage
  failures.

## Compatibility Impact

SQL-visible behavior should not change. The handler only uses the new storage
lookup for key shapes where byte-exact key-image equality is already safe, and
all broader exact/prefix/range semantics continue through MariaDB key
comparison.

## Single-File And Lifecycle Impact

No file-format changes. The durable implementation reads existing index-entry
pages and row-state tombstones. The volatile implementation reads the existing
process-local table vectors. No new sidecars or lifecycle objects are added.

## Storage-Engine Routing Impact

The fast path applies to MyLite-routed durable tables and runtime-volatile
MEMORY/HEAP tables when the active key matches the guarded raw exact shape.
Requested engine routing remains unchanged.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to two small storage helpers and
handler wiring.

## Tests And Verification

- Add storage unit coverage for durable exact index-entry row-id lookup over a
  live row, stale deleted row, replacement row, missing key, and invalid input.
- Extend routed storage smoke coverage through existing primary-key point reads
  and duplicate secondary-key iteration.
- Run the storage unit tests, storage-engine smoke, compatibility harness group,
  performance baseline, changed-line formatting checks, and `git diff --check`.
- A local post-change performance-baseline run measured direct/prepared
  primary-key point selects at `2262.830` / `2274.760` us/op and
  direct/prepared primary-key updates at `7661.780` / `16310.730` us/op.
  Additional reruns were noisier, so these are local machine-dependent evidence,
  not release thresholds.

## Acceptance Criteria

- Durable and volatile storage expose exact index-entry row-id lookup helpers.
- The handler uses the helper only for guarded raw exact unique integer reads.
- Matching lookups produce a one-row cursor and read the selected row.
- Missing lookups return normal key-not-found behavior.
- Existing storage-engine compatibility tests pass and the performance baseline
  is recorded.

## Risks

- The API deliberately does not solve collation-sensitive exact string lookup;
  using byte equality there would be a compatibility bug.
- This remains scan-based over index-entry pages. B-tree pages are still the
  long-term structure needed for SQLite-like indexed lookup complexity.
