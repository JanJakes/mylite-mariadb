# Autoincrement Row Storage

## Problem

MyLite can store and scan keyless non-BLOB rows, but the handler still rejects
all writes when `TABLE::next_number_field` is present. That blocks common
MySQL/MariaDB table definitions such as:

```sql
CREATE TABLE posts (
  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  title VARCHAR(255) NOT NULL
) ENGINE=InnoDB;
```

Because MyLite routes `ENGINE=InnoDB` to the MyLite storage engine, this common
schema should work for the first supported row-storage path without adding
general secondary-index behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/table.cc:9475-9483` calls
  `handler::update_auto_increment()` from `TABLE::update_generated_fields()`
  before `write_row()`.
- `mariadb/sql/table.cc:7815-7824` marks the auto-increment field readable and
  writable during inserts because the generated-value path stores into the
  field.
- `mariadb/sql/handler.cc:4242-4314` documents the generated-value rules:
  NULL or zero values generate a new value unless
  `NO_AUTO_VALUE_ON_ZERO` applies; explicit positive values can move the next
  generated value forward.
- `mariadb/sql/handler.cc:4434-4454` calls the engine's
  `get_auto_increment()` and then rounds the result with the session
  `auto_increment_offset` and `auto_increment_increment` settings.
- `mariadb/sql/handler.cc:4573-4668` shows the default
  `handler::get_auto_increment()` implementation derives the next value by
  reading the highest indexed row. MyLite cannot use that path until it owns
  index access methods.
- `mariadb/sql/handler.h:4454-4460` exposes `get_auto_increment()` as the
  engine override point.
- `mariadb/sql/handler.h:5249-5252` notes that duplicate-key returns must leave
  the duplicate row readable enough for an error message.
- `mariadb/storage/mylite/ha_mylite.cc:353-368` currently rejects any table
  with keys or `next_number_field` before appending the raw MariaDB record
  image.

## Design

Add a bounded MyLite autoincrement path:

- Override `ha_mylite::get_auto_increment()` so MariaDB's existing
  `update_auto_increment()` flow can fill generated values before
  `ha_mylite::write_row()`.
- Store the durable next candidate in append-only autoincrement state pages in
  the primary `.mylite` file, keyed by catalog table id.
- Advance durable state from `write_row()` after a successful generated or
  explicit positive insert. Gaps are acceptable and match normal
  MySQL/MariaDB expectations; MyLite does not promise gapless sequences.
- Permit writes for tables whose only key is a single-part key over the
  `AUTO_INCREMENT` column. Continue rejecting arbitrary keys, compound
  autoincrement keys, and non-autoincrement keys.
- Check duplicate values for that single autoincrement key by scanning existing
  raw row images and comparing the auto field through MariaDB's `Field`
  object. This is a narrow duplicate gate, not general index support.
- Preserve raw MariaDB row-image storage. The row format does not gain typed
  column encoding in this slice.

The state page approach avoids changing the existing catalog table-definition
record and keeps old table ids isolated: dropped/recreated tables get new table
ids, and old autoincrement state pages are ignored like old row pages.

## Non-Goals

- General primary-key, unique-key, secondary-index, or ordered-index reads.
- Compound autoincrement keys where the auto column is not the complete key.
- `ALTER TABLE ... AUTO_INCREMENT=N`; `TRUNCATE TABLE` reset is handled by the
  truncate table lifecycle slice.
- Transaction rollback of consumed autoincrement values.
- Cross-process autoincrement locking guarantees.
- BLOB/TEXT rows, update/delete, and copy `ALTER` rebuilds.

## Compatibility Impact

This moves a common routed MySQL/MariaDB schema shape from planned to partial:
single-column `AUTO_INCREMENT` keys can be inserted, scanned, closed, and
reopened through MyLite storage when row values otherwise fit the current
non-BLOB raw-record path.

Compatibility remains partial because MyLite still lacks general index access,
transactional rollback, update/delete, and `ALTER` handling.

## Single-File Impact

Durable autoincrement state is stored in the primary `.mylite` file. No
MariaDB sidecars, plugin files, or MyLite companion files are introduced.

Autoincrement state pages are append-only for now. Free-space reclamation and
checkpoint compaction remain part of later storage-management work.

## File Format Impact

Add a checksummed autoincrement state page type:

- page magic,
- page type and page version,
- storage format version,
- checksum algorithm,
- page id,
- table id,
- next candidate value,
- checksum.

The storage capability mask gains an autoincrement-state flag. The global file
format version remains unchanged for this early development branch because the
current reader understands the new page type and existing page layouts are
unchanged. Pre-slice binaries are not expected to read post-slice files.

## Test Plan

- Add first-party storage tests for reading the default next value, advancing
  it, preserving the maximum value, and ignoring dropped table ids after
  recreate.
- Add embedded storage-engine coverage for:
  - `ENGINE=InnoDB` auto-increment primary-key table creation,
  - generated inserts,
  - explicit positive inserts advancing later generated values,
  - duplicate explicit values failing,
  - close/reopen preserving the next generated value,
  - no forbidden durable sidecars.
- Run:
  - `cmake --build --preset dev --target format`
  - `cmake --build --preset dev && ctest --preset dev --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev && ctest --preset storage-smoke-dev --output-on-failure`
  - `cmake --build --preset embedded-dev && ctest --preset embedded-dev --output-on-failure`
  - `cmake --build --preset dev --target format-check`
  - `git diff --check`
  - `cmake --build --preset dev --target tidy`

## Acceptance Criteria

- Supported autoincrement rows persist entirely in the `.mylite` file.
- Generated values and explicit high values survive close/reopen.
- Duplicate explicit autoincrement values fail for the supported single-column
  key shape.
- Unsupported key shapes still fail before row publication.
- Compatibility, storage architecture, and roadmap docs accurately describe the
  partial support.

## Implementation Status

Implemented in the storage package and MyLite handler:

- `mylite_storage_read_auto_increment()` and
  `mylite_storage_advance_auto_increment()` read and append checksummed
  autoincrement state pages keyed by catalog table id.
- `ha_mylite::get_auto_increment()` feeds MariaDB's
  `handler::update_auto_increment()` path from durable MyLite state.
- `ha_mylite::write_row()` now calls `update_auto_increment()` under MariaDB's
  normal handler contract, permits the single-column autoincrement key shape,
  performs scan-based duplicate checks for that key, advances durable state,
  and then appends the raw row image.
- Storage unit coverage checks default state, monotonic advancement,
  corruption detection, and drop/recreate table-id isolation.
- Storage-engine smoke coverage checks generated values, explicit high values,
  duplicate failures, close/reopen persistence, `ENGINE=InnoDB` routing, and
  sidecar absence.
- The truncate table lifecycle slice resets autoincrement to the first
  generated value during `TRUNCATE TABLE`.

## Risks

- Duplicate checks are scan-based until real indexes exist, so this is correct
  for the narrow shape but not a scalable index implementation.
- Autoincrement advancement is not transaction-aware yet; later transaction
  work must decide whether and how to recover or preserve consumed values.
- Cross-process writers can race without the future locking slice.
