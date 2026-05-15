# Warnings API

## Goal

Expose retained MariaDB warning rows and their row count through the primary
`libmylite` database handle after direct and prepared SQL execution.

This advances the warning part of the SQL execution API without exposing raw
MariaDB connection or statement handles.

## Non-Goals

- Do not expose `MYSQL *`, `MYSQL_STMT *`, `THD *`, or MariaDB diagnostics
  objects.
- Do not implement `GET DIAGNOSTICS`.
- Do not preserve warnings across a later successful statement.
- Do not claim full warning retention beyond MariaDB's own stored warning rows.
- Do not change SQL mode, strictness, or warning-producing semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h` declares `mysql_warning_count(MYSQL *)` and the
  direct-result APIs needed to read `SHOW WARNINGS`.
- `mariadb/libmysqld/libmysql.c` implements `mysql_warning_count()` by
  returning `mysql->warning_count` for the embedded API.
- `mariadb/libmysqld/libmysql.c` updates the connection warning count while
  reading prepared-statement prepare and result packets. MyLite treats that
  value as a hint for whether to issue `SHOW WARNINGS`, not as a separate
  total-count API.
- `mariadb/sql/sql_error.h` documents `Warning_info::warn_count()` as the value
  used by `@@warning_count` and notes it can be higher than the stored warning
  list when `max_error_count` limits retained rows.
- `mariadb/sql/sql_error.h` marks `SHOW WARNINGS`, `SHOW ERRORS`, and
  `GET DIAGNOSTICS` as diagnostics statements that leave the statement area
  read-only while reading warnings.

## Design

Add the public warning API already documented in
`docs/api/libmylite-c-api.md`:

- `mylite_warning_count(mylite_db *)`,
- `mylite_warning(mylite_db *, unsigned, mylite_warning_level *, unsigned *,
  const char **)`.

The database handle stores:

- the retained row count from `SHOW WARNINGS`;
- the structured rows returned by `SHOW WARNINGS`, each with level, numeric
  code, and message text.

`mylite_warning_count()` returns the number of stored rows. `mylite_warning()`
uses a zero-based index into those rows and returns `MYLITE_NOTFOUND` for
indexes beyond the stored diagnostics.

Direct execution captures warnings after MyLite has read the statement result,
affected rows, and insert id. Prepared execution captures warnings immediately
for non-result statements and after `mylite_step()` reaches `MYLITE_DONE` for
result-producing statements. While a prepared result set is still producing
rows, warning access reflects the previous completed statement.

## Compatibility Impact

This moves warnings from planned to partial coverage:

- direct execution exposes retained structured `SHOW WARNINGS` rows after
  successful statements;
- prepared non-result statements expose warnings after `mylite_step()` returns
  `MYLITE_DONE`;
- prepared result statements expose warnings after the result set is drained;
- failed direct execution, failed prepare, and failed prepared execute paths
  retain structured warning/error rows before a prepared result set is active.

Errors continue to use existing MariaDB errno, SQLSTATE, and message APIs.
Fetch-time failure warning capture remains out of scope for this slice.

## Single-File And Storage Impact

No file-format or storage-engine change is required. Warning state is
connection/session diagnostics state owned by the `mylite_db` handle.

## Embedded Lifecycle And API

Warning row message pointers are owned by the database handle and remain valid
until the next successful statement, `mylite_close()`, or a MyLite API call that
updates warning storage.

`mylite_warning(NULL, ...)` and missing output pointers return
`MYLITE_MISUSE`. `mylite_warning_count(NULL)` returns zero.

## Build, Size, And Dependencies

No new dependency is added. The implementation uses MariaDB's existing
embedded query/result functions.

The size impact should be negligible because direct execution already links the
result APIs and prepared statements already link statement execution.

## Test Plan

1. Extend public API tests for `NULL` warning handles and output pointers.
2. Add embedded tests for:
   - direct SQL retained warning rows;
   - warnings being cleared by a later clean statement;
   - prepared non-result warning capture;
   - prepared result warnings becoming visible after result exhaustion;
   - failed direct, prepare, and prepared execute warning rows;
   - missing warning indexes returning `MYLITE_NOTFOUND`.
3. Add a compatibility harness group for warning enumeration.
4. Run `dev`, `embedded-dev`, `storage-smoke-dev`, the warning compatibility
   group, format, tidy, diff checks, and size report.

## Acceptance Criteria

- The public header exposes warning levels and warning accessors.
- Successful direct and prepared execution captures MariaDB warning rows.
- Failed direct, prepare, and prepared execute paths capture MariaDB
  warning/error rows where no prepared result set is active.
- Structured warning rows expose level, code, and message.
- Warning rows are database-owned and remain valid until replaced.
- Docs and compatibility matrix mark warning enumeration as partial, with
  limits around stored rows and fetch-time failures explicit.

## Risks And Open Questions

- `SHOW WARNINGS` returns at most MariaDB's retained warning list. MyLite keeps
  the retained rows rather than promising a separate total count.
- Prepared result warnings may not be final until the result is drained; this
  slice exposes them at `MYLITE_DONE` rather than during active row iteration.
