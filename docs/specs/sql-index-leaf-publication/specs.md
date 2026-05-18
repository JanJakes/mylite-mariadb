# SQL Index Leaf Publication

## Problem

The storage layer can rebuild single-level index leaf pages and use them with
an append-tail overlay for exact byte-key lookups. SQL paths still do not
publish those roots naturally, so normal routed DDL and copy rebuilds keep using
append-log exact scans unless a storage unit test explicitly calls the rebuild
API.

The next integration step is to publish leaf roots at a low-risk SQL lifecycle
point: after successful copy-rebuild table/index DDL finalizes the durable table
identity. This gives rebuilt tables a useful exact-lookup base snapshot without
attempting per-row maintained B-tree updates.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc::mysql_alter_table()` drives copy rebuilds for
  supported `ALTER TABLE`, standalone `CREATE INDEX`, and standalone
  `DROP INDEX` when the handler reports `HA_ALTER_INPLACE_NOT_SUPPORTED`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::check_if_supported_inplace_alter()`
  always returns `HA_ALTER_INPLACE_NOT_SUPPORTED`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` stores the rebuilt
  table definition before MariaDB copies rows into the new table.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rename_table()` handles the
  copy-rebuild rename sequence. The old logical table is first renamed to an
  alter backup name; the rebuilt table is later renamed to the final logical
  name.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_current_alter_info_for_key_rename()`
  already identifies the copy-rebuild command family used by related FK rename
  metadata code, but leaf publication needs a narrower index-specific trigger
  so ordinary FK and column rebuilds do not spend catalog space on optional
  roots.

## Design

- Add a MyLite handler helper that maps explicit rebuilt SQL key definitions to
  supported keys in the rebuilt catalog-backed table share and calls
  `mylite_storage_rebuild_index_leaf()` for matching key numbers.
- Call that helper after a successful non-backup `rename_table()` for
  standalone `CREATE INDEX` and `ALTER TABLE ... ADD KEY` / `ADD INDEX` /
  `ADD PRIMARY KEY` copy rebuilds.
- Do not publish roots for generated FK supporting keys, `DROP INDEX`, or
  `RENAME INDEX` in this slice. Those paths remain correct through append-log
  exact scan fallback, and avoiding optional root records keeps the current
  single-page catalog available for required table and FK metadata.
- Keep leaf publication opportunistic:
  - ignore `MYLITE_STORAGE_FULL` because oversized single-page leaves can still
    use append-log scan fallback, and because the current single-page catalog
    reserves headroom for required schema metadata before accepting optional
    index-root records;
  - ignore `MYLITE_STORAGE_UNSUPPORTED` because variable or mixed key sizes are
    outside the current leaf format;
  - ignore `MYLITE_STORAGE_NOTFOUND` because some rebuild flows can legitimately
    have no durable table/index target after a dropped key;
  - return real handler errors for I/O, corruption, misuse, or no-memory cases.
- Do not publish leaves for runtime-volatile MEMORY/HEAP tables or BLACKHOLE row
  discard tables.
- Do not maintain leaves on ordinary row insert/update/delete yet. The
  append-tail overlay keeps published roots correct after later mutations.

## Compatibility Impact

SQL-visible behavior should not change. Leaf roots are an internal exact-lookup
optimization for the existing guarded byte-exact full-key storage APIs.

## Single-File And Lifecycle Impact

Published leaf pages live in the primary `.mylite` file and are catalog-rooted
through existing index-root metadata. Failed leaf publication for unsupported or
oversized indexes, or for catalogs without enough reserved metadata headroom,
does not fail the DDL because the append-log scan fallback remains
authoritative.

## Public API And File-Format Impact

No public API or file-format change. This slice wires the existing storage API
into the handler lifecycle.

## Storage-Engine Routing Impact

The integration applies only to durable MyLite-routed tables. MEMORY/HEAP
volatile tables and BLACKHOLE row-discard tables keep their existing behavior.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to a small handler helper and test
assertions.

## Tests And Verification

- Add embedded storage-engine smoke assertions that standalone `CREATE INDEX`
  and `ALTER TABLE ... ADD KEY` copy rebuilds publish index-root metadata for
  supported fixed-width keys.
- Verify SQL indexed reads still work after publication, close/reopen, rename,
  and drop paths already covered by the existing test. Rename and drop paths use
  scan fallback unless a previous explicit add/create path left a usable root.
- Run the storage-engine smoke target, compatibility harness `storage-engine`,
  changed-line formatting checks, and `git diff --check`.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`

## Acceptance Criteria

- Successful standalone `CREATE INDEX` and explicit `ALTER TABLE ... ADD KEY`
  copy-rebuild DDL publishes leaf roots for supported fixed-width durable
  indexes when the current single-page catalog has reserved metadata headroom.
- Unsupported or oversized leaf publication does not fail otherwise successful
  SQL DDL.
- Existing routed index DDL behavior and sidecar gates remain unchanged.

## Risks

- This still does not maintain B-tree pages per row mutation.
- The helper depends on the handler table share matching the rebuilt table after
  final copy-rebuild rename; tests must cover representative standalone and
  ALTER-backed index DDL.
- Leaf roots are useful only for byte-exact lookup shapes until broader index
  navigation exists.
