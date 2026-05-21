# SQL Digest Trim

## Problem Statement

The default embedded archive still builds MariaDB's statement digest
normalization runtime. Statement digests collect parser tokens so Performance
Schema can expose normalized statement text and digest hashes. MyLite already
omits Performance Schema from the default embedded profile, so retaining the
digest normalizer adds code and per-session token buffers for a diagnostic
surface that is not part of application SQL execution, native storage, or the
public C API.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_digest.cc` implements parser-token collection,
  identifier/literal normalization, digest text rendering, and digest MD5
  calculation.
- `mariadb/sql/sql_digest.h` and `mariadb/sql/sql_digest_stream.h` define the
  retained storage structures used by parser and Performance Schema call sites.
- `mariadb/sql/sql_lex.cc` calls `digest_add_token()` and
  `digest_reduce_token()` only when a parser digest listener is installed.
- `mariadb/sql/sql_parse.cc` installs that listener only after
  `MYSQL_DIGEST_START()` returns a Performance Schema digest locker.
  Performance Schema is omitted from the default embedded profile, so this path
  is diagnostic-only for MyLite.
- `mariadb/sql/sql_class.cc` allocates `THD::m_token_array` when
  `max_digest_length > 0`; `mariadb/sql/sys_vars.cc` registers
  `max_digest_length` as a read-only startup variable with an upstream default
  of 1024.
- `mariadb/libmysqld/CMakeLists.txt` currently links `../sql/sql_digest.cc`
  into `libmariadbd.a`; the current stripped member is 57,056 bytes.

## Design

Add `MYLITE_WITH_SQL_DIGEST`, defaulting to `ON` for upstream-style builds and
forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.

When disabled, the embedded build links a small MyLite-owned
`mylite_sql_digest_disabled.cc` object in place of `sql_digest.cc`. The stub
preserves the externally referenced digest symbols, returns no-op token
collection, emits empty digest text, and returns a zero digest hash if a custom
call path asks for one.

MyLite startup also passes `--max-digest-length=0` so the embedded runtime does
not allocate per-session statement-digest token buffers. The variable remains
readable as compatibility evidence.

## Compatibility Impact

Statement digest output is explicitly unavailable in the default embedded
profile. This removes Performance Schema digest text/hash diagnostics, not SQL
parsing, statement execution, prepared statements, `EXPLAIN`, errors, warnings,
JSON SQL functions, GEOMETRY/GIS, native storage engines, or the public C API.

## Database-Directory And Lifecycle Impact

None. Statement digest trimming changes in-memory diagnostics only. It does
not add durable files, temporary files, locks, metadata, or runtime
directories.

## Public API Impact

None. `libmylite` headers and symbols are unchanged. Result callbacks, prepared
statements, errors, and warning retrieval keep their existing behavior.

## Native Storage Impact

None. Native engine routing, table files, transaction behavior, and recovery
behavior are unchanged.

## Binary-Size Impact

Measured after a clean embedded relink with
`tools/mariadb-embedded-build measure`, this reduces the stripped archive from
27,095,640 bytes / 25.84 MiB to 27,039,160 bytes / 25.79 MiB, saving 56,480
bytes with no member-count change. The pre-strip archive drops from
27,689,312 bytes / 26.41 MiB to 27,627,712 bytes / 26.35 MiB, saving 61,600
bytes before symbol stripping.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_SQL_DIGEST=OFF` appears in the embedded cache summary.
- Confirm `sql_digest.cc.o` is absent and
  `mylite_sql_digest_disabled.cc.o` is present in `libmariadbd.a`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The embedded baseline configures `MYLITE_WITH_SQL_DIGEST=OFF`.
- The default embedded archive replaces `sql_digest.cc.o` with
  `mylite_sql_digest_disabled.cc.o`.
- `@@max_digest_length` reports `0` in the embedded runtime.
- SQL execution, prepared statements, ordinary diagnostics, and existing
  compatibility tests continue to pass.
- Architecture, compatibility, API, roadmap, and size-profile docs describe the
  unsupported diagnostic surface and measured size impact.

## Risks And Unresolved Questions

- The parser still references digest hook symbols, so this slice must keep
  no-op symbols rather than deleting the digest API.
- A custom embedded build that re-enables Performance Schema while leaving
  `MYLITE_WITH_SQL_DIGEST=OFF` will expose empty digest diagnostics. The
  default MyLite profile keeps Performance Schema omitted.
