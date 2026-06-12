# Ownerless Prepared DML Text Execution

## Problem

Ownerless prepared no-result DML currently creates and closes a native MariaDB
`MYSQL_STMT` inside every `mylite_step()` call. The previous profiling slice
showed that this protects the ownerless statement boundary, but also makes
native prepare/close lifecycle cost a first-order prepared-write performance
target.

Keeping a native prepared handle alive across public MyLite calls is not safe
without a directory-owned peer-join barrier. A different optimization is to
keep the public prepared-statement object and parameter APIs, but execute the
covered ownerless no-result DML subset through MariaDB's length-aware text
query path after rendering bound values into SQL literals. That avoids
long-lived process-local native prepared state.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/mysql.h` declares `mysql_real_query(MYSQL *, const char *,
  unsigned long)` and `mysql_real_escape_string(MYSQL *, char *, const char *,
  unsigned long)`.
- `mariadb/libmysqld/libmysql.c` implements `mysql_query()` by calling
  `mysql_real_query()` with `strlen()`, so MyLite should use
  `mysql_real_query()` directly for generated SQL whose buffer and length are
  already known.
- `mariadb/sql-common/client.c` implements `mysql_real_query()` by sending the
  caller-provided query bytes and length, then reading the query result.
- `mariadb/sql-common/client.c` implements `mysql_real_escape_string()` so it
  chooses quote-only escaping when `SERVER_STATUS_NO_BACKSLASH_ESCAPES` is
  active and ordinary slash escaping otherwise. The embedded path therefore has
  a MariaDB-owned escaping routine that follows the current connection charset
  and SQL mode.
- `mariadb/libmysqld/libmysql.c` implements `mysql_stmt_prepare()` and
  `mysql_stmt_execute()` with native statement handle state. The existing
  ownerless prepared-DML path deliberately closes this state before
  `mylite_step()` returns.
- `packages/libmylite/src/database.cc` already counts parameter markers with
  `next_sql_token()`, which skips ordinary comments and quoted tokens, and
  already defers native prepare only for no-result ownerless `INSERT`,
  `UPDATE`, `DELETE`, and `REPLACE` statements without `RETURNING`.
- The ownerless direct SQL path already performs the required ownerless
  lifecycle sequence: page-log pressure checks, ownerless runtime statement
  tracking, statement locks, external page refresh, dictionary DDL begin/finish,
  consistent-snapshot pinning, page-write tracking, query execution, result
  draining, temporary-table and transaction state updates, page-write lock
  release, read-LSN advancement, and statement-boundary reclamation.

## Design

Add a text execution path for ownerless prepared no-result DML statements that
already use `ownerless_native_prepare_per_step`.

At `mylite_step()` time, after the existing ownerless locks, refreshes, and
dictionary gates are in place:

1. Rebuild the original prepared SQL into a new SQL string by replacing each
   marker token with the current bound value.
2. Render `NULL` as `NULL`.
3. Render signed and unsigned 64-bit integer bindings as decimal numeric
   literals.
4. Render finite double bindings as decimal numeric literals with enough
   precision for round-trip value preservation.
5. Render text bindings as single-quoted string literals escaped through
   `mysql_real_escape_string()` against the active embedded connection.
6. Render blob bindings as hexadecimal binary literals so embedded NUL bytes do
   not depend on C-string termination or connection text escaping.
7. Execute the rendered SQL with `mysql_real_query()` and the rendered length.
8. Drain any response through the existing result-drain path and keep the same
   post-statement ownerless bookkeeping used by prepared native execution.

This slice intentionally does not add a native prepared-handle cache. If text
execution proves insufficient, a later cache must first add a directory-owned
peer-join barrier or statement-cache lease that prevents a peer from entering
while process-local native prepared state remains alive.

## Scope

Covered:

- Ownerless read/write opens.
- Prepared no-result `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` statements
  without `RETURNING`.
- Public MyLite bindings for `NULL`, `int64`, `uint64`, finite `double`, text,
  and blob values.
- Existing ownerless statement locks, page refresh, transaction state,
  dictionary state, page-write release, and pressure handling.

Non-goals:

- Prepared result statements.
- Prepared DDL, `CALL`, server-surface statements, or statements rejected by
  existing ownerless policy.
- Native prepared-handle caching.
- A new SQL parser beyond the existing token scanner used for parameter-marker
  counting and ownerless policy.

## Compatibility Impact

The public C API shape does not change. The optimization preserves the
statement/parameter API contract for the covered ownerless DML subset while
moving execution from native prepared protocol to MariaDB's text query path.
Diagnostics still come from MariaDB at `mylite_step()` time, which matches the
current deferred-native ownerless behavior.

Unsupported or risky value forms must fail explicitly rather than fall back to
unsafe SQL text. In particular, non-finite doubles should return `MYLITE_MISUSE`
or `MYLITE_ERROR` unless a MariaDB-compatible literal representation is
designed and tested.

## Native Storage Impact

No native InnoDB file format, redo, undo, page-version WAL, dictionary
generation, or checkpoint layout changes. The same SQL reaches MariaDB/InnoDB
inside the existing ownerless statement boundary, but without creating a native
prepared statement handle for each execution.

## Database Directory And Lifecycle Impact

No new durable files, shared-memory segments, or directory metadata are added.
The optimization avoids a peer-join barrier by not retaining native prepared
state outside a statement. Ownerless process slots, locks, page-version pins,
and checkpoint scheduling keep their existing lifetimes.

## Build, Size, License, And Dependency Impact

No new dependency is introduced. The change adds first-party SQL rendering code
in `packages/libmylite/src/database.cc` and uses MariaDB C API functions already
linked into the embedded profile. Binary-size impact should be negligible.

## Test And Verification Plan

- Add focused embedded prepared-statement coverage for ownerless prepared DML
  executed through text rendering:
  - integer, unsigned integer, double, NULL, text with quotes/backslashes, empty
    text, blob with embedded NUL bytes, and repeated reset/reexecute;
  - text escaping under `NO_BACKSLASH_ESCAPES`;
  - duplicate-key error recovery and later successful reexecution;
  - parameter markers in string literals and ordinary comments remain literal
    text, not bind markers.
- Extend the performance probe or existing stats assertions to verify eligible
  ownerless prepared writes emit zero native prepare/close calls when stats are
  enabled.
- Run focused prepared statement CTest under `php-embedded-prod`.
- Run stats-enabled and stats-off production performance probes to compare the
  ownerless prepared insert path before and after text execution.
- Run focused `checksum-stress` controls with prepared writers.
- Run production build guards, format check, and whitespace check.

## Acceptance Criteria

- Eligible ownerless prepared no-result DML executes without native
  `mysql_stmt_prepare()` or `mysql_stmt_close()` per step.
- Covered bound values round-trip through MariaDB with expected SQL results,
  including binary-safe blob payloads.
- Existing ownerless statement boundaries and post-statement bookkeeping remain
  in force.
- Prepared DML reset/reexecution and error recovery still pass.
- Focused ownerless prepared-writer stress still passes.
- Docs record why this text path is preferred before a native prepared-handle
  cache.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_prepared_statement_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-prepared-statement$' --output-on-failure` passed after
  the ownerless prepared-DML test was expanded to cover unsigned integers,
  doubles, `NULL`, quote/backslash text under `NO_BACKSLASH_ESCAPES`, empty
  text/blob values, embedded-NUL blobs, duplicate-key recovery, repeated
  reset/reexecute, and non-marker `?` characters in string literals and
  ordinary comments.
- A reduced stats-enabled production performance probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`,
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`, and
  `MYLITE_PERF_INSERT_ITERATIONS=100` reported
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_prepare_calls=0`,
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_prepare_ms=0.000`,
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_close_calls=0`,
  and
  `mylite_perf_ownerless_insert_autocommit_prepared_step_native_close_ms=0.000`,
  with all matching per-insert summaries at `0.000`. The same run reported
  ordinary autocommit inserts at `2640.23 ops/s`, ownerless autocommit inserts
  at `389.66 ops/s`, and an ownerless/ordinary ratio of `0.1476`.
- A reduced stats-off production performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=500` reported ordinary transaction inserts at
  `4115.05 ops/s`, ownerless transaction inserts at `1506.99 ops/s`, ordinary
  autocommit inserts at `4042.51 ops/s`, ownerless autocommit inserts at
  `1349.90 ops/s`, and an ownerless autocommit ratio of `0.3339`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- Focused one-round `checksum-stress` controls passed with
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_PREPARED_WRITERS=0`, `1`, and `2`, each
  under a 120-second timeout.

## Risks And Follow-Up

- Text rendering must not treat `?` inside strings or ordinary comments as a
  parameter marker.
- Literal rendering can diverge from native prepared protocol for edge cases
  such as non-finite doubles, character-set edge cases, or context-sensitive
  parameter typing. Unsupported values should be explicit until covered.
- Text execution reparses SQL each time. It should still remove the measured
  native prepare/close lifecycle cost, but a later profile may show parser cost
  or page-version publication as the next bottleneck.
- A future native prepared cache remains possible only after a directory-owned
  peer-join barrier or cache lease is designed and proven.
