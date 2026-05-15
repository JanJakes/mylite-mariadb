# Collation Restart Matrix

## Problem

MyLite now restores MariaDB compiled charset definitions across embedded
shutdown, and the storage-smoke suite covers `utf8mb4_unicode_ci`. That proves
the original UCA crash path, but it still leaves the roadmap's broader
collation-suite gap open.

This slice adds a bounded collation matrix that exercises representative
compiled, UCA, binary, and one-byte charset collations through indexed MyLite
tables across close/reopen cycles.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysys/charset.c` registers compiled collations through
  `add_compiled_collation()` and lazily prepares them through
  `get_internal_charset()`.
- `mariadb/strings/ctype-utf8.c` defines compiled `utf8mb4_general_ci` and
  `utf8mb4_bin` handlers.
- `mariadb/strings/ctype-uca.c` defines `utf8mb4_unicode_ci` and
  `utf8mb4_unicode_520_ci` UCA handlers.
- `mariadb/strings/ctype-latin1.c` and `sql/share/charsets/latin1.xml` define
  `latin1_swedish_ci`, the traditional MariaDB/MySQL default latin1 collation.
- The MyLite storage handler stores and reads MariaDB key images, then relies on
  MariaDB collation handlers for indexed lookup and duplicate checks.

## Design

Extend the storage-smoke executable with one matrix test. The test creates a
file-backed schema and one indexed table for each representative collation:

- `utf8mb4_general_ci`;
- `utf8mb4_bin`;
- `utf8mb4_unicode_ci`;
- `utf8mb4_unicode_520_ci`;
- `latin1_swedish_ci`.

Each table uses a unique indexed `name` column and a secondary indexed `slug`
column. The test inserts representative ASCII values, closes the database,
reopens it twice, then checks:

- table collation metadata;
- duplicate-key rejection on the unique key;
- indexed lookup through `FORCE INDEX (name_key)`;
- the existing durable sidecar gate after each close.

This is a compatibility matrix, not a semantics matrix. It proves the selected
collation handlers remain initialized and usable for MyLite index paths across
embedded restarts; it does not claim full linguistic comparison coverage.

## Supported Scope

- The five representative collations above.
- File-backed routed `ENGINE=InnoDB` tables.
- Unique and secondary index paths after close/reopen.
- Metadata checks through `INFORMATION_SCHEMA.TABLES`.

## Non-Goals

- Exhaustive MariaDB collation coverage.
- Locale-specific ordering or accent/case-sensitivity assertions.
- Non-ASCII fixture data beyond what later application suites require.
- Public API, storage format, or build-profile changes.

## Compatibility Impact

Application-schema and storage-engine compatibility remain partial, but MyLite
gains evidence that several common MariaDB collations survive embedded restart
and remain usable for supported index paths.

## Single-File And Embedded-Lifecycle Impact

The test uses the existing file-backed storage-smoke lifecycle and must leave
only the primary `.mylite` file after close.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

No routing behavior changes. The test uses existing `ENGINE=InnoDB` to MyLite
routing.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

No build-profile changes are expected.

## License Or Dependency Impact

None.

## Test And Verification Plan

- Add the collation matrix storage-smoke test.
- Run the focused storage-engine smoke test.
- Run `tools/mylite-compat-harness report application-schema storage-engine`.
- Run format, tidy, dev, embedded, and storage-smoke checks before commit.

## Acceptance Criteria

- Every matrix table records and reports the expected collation.
- Duplicate-key checks and indexed lookups pass for each matrix table after
  repeated close/reopen cycles.
- Sidecar gates pass after each close.
- Compatibility and roadmap docs describe the bounded matrix without claiming
  exhaustive collation support.

## Risks

- This catches restart and index-handler regressions, not full string-comparison
  semantics. Broader application data and MariaDB comparison suites still need
  separate coverage.
