# MTR Storage Large-Value Prefix Smoke

## Problem

The raw storage-routed MTR suite covers representative DDL, DML, generated
columns, constraints, and indexes, but it does not yet have a dedicated
large-value smoke for `TEXT`/`BLOB` row payloads and bounded prefix indexes.
Those surfaces are already covered by first-party C storage tests; the MTR
storage profile should also prove them through MariaDB SQL and result files.

## Source Findings

Base: MariaDB `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.h:200` advertises
  `HA_CAN_INDEX_BLOBS` for the MyLite handler.
- `mariadb/storage/mylite/ha_mylite.cc:5277` and `:5292` accept bounded
  BLOB/TEXT key parts and reject unbounded zero-length BLOB/TEXT key parts.
- `mariadb/sql/field.h:4525` reports BLOB/TEXT fields with `HA_BLOB_PART`.
- `mariadb/sql/key.cc:145` and `:228` handle BLOB/TEXT key tuple copy and
  comparison through MariaDB key helpers.
- `docs/specs/blob-text-prefix-indexes/specs.md` records the implemented
  design for routed BLOB/TEXT prefix indexes; this slice adds raw MTR coverage
  rather than new production behavior.

## Scope

- Add one MyLite-owned storage-routed MTR test for large `LONGTEXT` and
  `LONGBLOB` payloads.
- Cover routed `ENGINE=InnoDB` and explicit `ENGINE=MYLITE` tables.
- Cover forced reads through bounded `TEXT` and `BLOB` prefix indexes.
- Cover stale prefix-entry filtering after a large-value update.
- Cover uniqueness enforcement for a bounded `TEXT` prefix index.
- Keep durable sidecar checks for the schema.

## Non-Goals

- Do not implement new storage behavior.
- Do not cover full or oversized BLOB/TEXT indexes, generated BLOB/TEXT prefix
  indexes, FULLTEXT, SPATIAL, vector, expression, or hash index classes.
- Do not benchmark large-value paths in MTR.

## Compatibility Impact

This adds compatibility evidence for existing partial BLOB/TEXT value and
bounded prefix-index support in the storage-routed MTR profile. It does not
change the supported SQL surface.

## Single-File And Lifecycle Impact

The test must not create durable MariaDB sidecars. Large row payloads and
prefix index entries should remain in the primary `.mylite` file plus allowed
MyLite recovery companions.

## Test And Verification Plan

- Add `mylite.routed_storage_large_values` to the storage MTR list.
- Run the new test through `tools/mylite-mtr-harness run-storage`.
- Run the full storage-routed MTR list.
- Run MTR coverage inventory and shell/diff checks.

## Acceptance Criteria

- Large `TEXT` and `BLOB` values are readable through routed `InnoDB` and
  explicit MyLite tables.
- Forced prefix-index reads find the expected large-value rows.
- Updating a large value hides stale prefix index entries and exposes the new
  prefix entry.
- A bounded unique `TEXT` prefix index rejects a duplicate live prefix.
- Sidecar checks pass before and after cleanup.
