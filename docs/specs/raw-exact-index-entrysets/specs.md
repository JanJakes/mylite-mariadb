# Raw Exact Index Entrysets

## Problem

Exact-key and prefix reads now build filtered handler cursors, and unique
integer full-key lookups use a storage helper that returns one matching row id.
Non-unique integer equality reads still ask durable or volatile storage for
every live entry in the index, then discard non-matching entries in the handler.
That keeps common secondary-index point reads allocation-heavy even when byte
equality is safe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` calls
  `ha_index_read_map()` for a range start, and `handler::read_range_next()` uses
  `ha_index_next_same()` for equality ranges.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` and
  `ha_mylite::index_read_idx_map()` pass `HA_READ_KEY_EXACT` and
  `HA_READ_PREFIX` filters into `build_index_cursor()`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  limits byte-exact key comparisons to integer-family key parts.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  and
  `mariadb/storage/mylite/mylite_volatile_rows.cc::mylite_volatile_read_index_entries()`
  materialize all live entries for an index before handler filtering.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  and `mylite_volatile_find_index_entry()` prove storage can scan for exact key
  bytes without allocating unrelated entries, but deliberately return only the
  first row id for unique-key lookups.

## Design

- Add durable and volatile storage APIs that return a
  `mylite_storage_index_entryset` containing only live entries whose key image is
  byte-equal to a supplied full key.
- Use the new entryset API from the handler only for full-key, non-nullable
  integer-family keys. Keep collation-sensitive strings, nullable keys, partial
  prefixes, and general range reads on the existing MariaDB key-tuple comparison
  path.
- Keep the existing one-row lookup for unique raw exact keys, since it avoids
  allocation entirely.
- For non-unique raw exact reads, build the handler cursor from the exact
  entryset, sort matching entries with the existing key/row-id comparator, and
  let `index_next_same()` keep enforcing the range boundary before row
  materialization.

## Compatibility Impact

SQL-visible behavior should not change. The new storage path is restricted to
key shapes where byte-equal storage keys match MariaDB key equality for the
covered full-key reads. Broader exact, prefix, nullable, and collation behavior
continues to use `key_tuple_cmp()`.

## Single-File And Lifecycle Impact

No file-format changes. Durable storage still scans existing append-only
index-entry pages and row-state tombstones. Volatile storage still scans
process-local row vectors. No new sidecars, lifecycle objects, or recovery
rules are introduced.

## Storage-Engine Routing Impact

The optimization applies to MyLite-routed durable tables and runtime-volatile
MEMORY/HEAP tables when the active lookup uses a supported raw exact full key.
Requested engine routing remains unchanged.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to small exact-entryset helpers and
handler selection logic.

## Tests And Verification

- Add storage unit coverage for durable exact entrysets over live duplicate
  secondary entries, stale deleted entries, replacement entries, missing keys,
  and invalid input.
- Add storage-smoke SQL coverage for durable and runtime-volatile non-unique
  integer secondary-index exact reads, including duplicate-row iteration and
  missing-key behavior.
- Run storage unit tests, storage-engine smoke, compatibility harness group,
  performance baseline, changed-line formatting checks, and `git diff --check`.
- A local performance-baseline run after the storage-engine harness measured
  direct/prepared primary-key point selects at `12656.850` / `13505.830` us/op
  and direct/prepared primary-key updates at `49181.120` / `100249.720` us/op.
  That harness exercises unique primary-key lookups rather than this new
  non-unique secondary-index path, and the run was noisy, so it is sanity
  evidence only.

## Acceptance Criteria

- Durable and volatile storage expose exact-entryset helpers that return only
  matching live entries.
- The handler uses the helper only for guarded raw exact full-key reads where
  multiple rows may match.
- Unique raw exact reads keep the allocation-free single-row lookup path.
- Non-unique exact reads return all matching rows and no non-matching rows.
- Existing routed storage compatibility tests pass.

## Risks

- This is still scan-based over append-only index-entry pages. It reduces
  allocation and handler filtering, but it is not a substitute for B-tree
  navigation.
- Widening the guard to strings, nullable keys, or partial prefixes would be a
  compatibility risk unless storage delegates comparison to MariaDB key
  semantics.
