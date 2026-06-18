# Ownerless Pressure Host-File Export Policy

## Problem

Ownerless active-reader pressure intentionally returns `MYLITE_BUSY` for
supported write statements when retained page-version WAL is at the configured
soft limit. Host-file export SQL is a deliberately unsupported server surface:
it can write caller-named files outside MyLite's result API and outside the
database directory lifecycle. Pressure classification must not turn those
policy failures into retryable pressure-limit errors.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:8927-8956` parses both ordinary
  `SELECT ... INTO` and CTE `WITH ... SELECT ... INTO` forms.
- `mariadb/sql/sql_yacc.yy:13378-13397` routes `INTO OUTFILE` and
  `INTO DUMPFILE` into file-export result sinks.
- `mariadb/sql/sql_class.h:6327-6338` documents the non-database file path
  represented by `sql_exchange`, including `INTO OUTFILE`.
- `mariadb/sql/sql_class.cc:3899-3916` contains MyLite's embedded archive
  stubs for `SELECT INTO OUTFILE` and `SELECT INTO DUMPFILE`, but policy
  rejection in `packages/libmylite/src/database.cc` should run before pressure
  throttling and before those result sinks are prepared.
- `packages/libmylite/src/database.cc:is_unsupported_select_file_statement()`
  classifies top-level `SELECT` and `WITH` statements with raw `INTO OUTFILE`
  or `INTO DUMPFILE` tokens as unsupported server-owned SQL.
- `packages/libmylite/src/database.cc:exec_impl()` applies
  `reject_unsupported_sql_policy()` before
  `enforce_ownerless_page_log_limit_policy()`.

## Scope And Non-Goals

In scope:

- Extend the existing `active-reader-pressure-write-policy` selector with
  direct `SELECT ... INTO OUTFILE`, direct `SELECT ... INTO DUMPFILE`, CTE
  `WITH ... SELECT ... INTO OUTFILE`, prepared `SELECT ... INTO OUTFILE`, and
  prepared `SELECT ... INTO DUMPFILE` assertions while retained WAL is at the
  ownerless pressure limit.
- Keep host-file export diagnostics aligned with the documented
  `server-owned SQL surface` policy.
- Update compatibility evidence to distinguish host-file exports from
  host-file imports.

Out of scope:

- Supporting host-file exports.
- Changing runtime policy order unless the focused coverage exposes a masking
  bug.
- Exhaustive export syntax coverage beyond the representative direct,
  prepared, `OUTFILE`, `DUMPFILE`, and CTE forms.

## Design

Reuse the existing pressure setup in
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create retained page-version WAL behind a live repeatable-read snapshot pin.
2. Reopen the writer with `ownerless_page_log_limit_bytes` equal to the
   retained WAL size.
3. Assert supported write classes still return `MYLITE_BUSY`.
4. Assert representative host-file export SQL returns `MYLITE_ERROR`,
   MariaDB errno `0`, and the `server-owned SQL surface` diagnostic instead of
   pressure busy.
5. Keep the existing post-pressure success checks and ownerless/native reopen
   checks.

## Compatibility Impact

No SQL surface becomes supported. The compatibility claim becomes more precise:
MyLite rejects host-file exports explicitly in ordinary policy coverage, and
ownerless retained-WAL pressure does not mask that rejection for representative
direct and prepared export forms.

## Directory And Lifecycle Impact

No directory layout change. The test proves the export path is rejected before
server file writers can create durable files outside the MyLite database
directory, even while ownerless retained-WAL pressure is active.

## Native Storage Impact

No native storage format change. The slice only adds coverage that host-file
export result sinks are not entered under pressure.

## Public API Impact

No public API change.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test And Verification Plan

- Configure and build the production embedded target with
  `cmake --preset embedded-prod` and
  `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test`.
- Run
  `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`.
- Run the corresponding production CTest shard containing the selector when
  practical.
- Configure/build `ownerless-test-hooks` and run the same focused selector to
  catch hook-build policy-order regressions.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Direct and prepared host-file export statements return `MYLITE_ERROR` with
  MariaDB errno `0` and the `server-owned SQL surface` diagnostic while the
  retained WAL pressure limit is reached.
- Existing supported write statements in the same selector still return
  `MYLITE_BUSY`.
- Existing final ownerless/native reopen and forced shared-memory rebuild
  checks still pass.

## Risks And Follow-Up

- Coverage is representative, not a complete grammar matrix for every
  `SELECT ... INTO` placement.
- Broader external MariaDB/RQG stress remains planned separately from this
  policy-order check.
