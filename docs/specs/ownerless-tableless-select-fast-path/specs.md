# Ownerless Tableless SELECT Fast Path

## Problem

Production PHPUnit timing on the ownerless branch showed ordinary tableless
probe queries, especially `SELECT 1`, paying ownerless read-visibility setup.
Those statements cannot read InnoDB table pages, but MyLite's ownerless SQL
policy treated every non-locking `SELECT`/`WITH` as eligible for page-version
reads. The direct path could therefore keep a baseline page-version pin after
`SELECT 1`, and the prepared path could enable ownerless page visibility while
the result cursor was active.

This is a performance slice only. It must not weaken committed visibility for
real table reads, locking reads, writes, DDL, or recovery.

## Source Findings

- MariaDB 11.8.6 base ref:
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_parse.cc:3988-4022` handles `SQLCOM_SELECT` with
  `all_tables == NULL` by checking database-level access and then calling
  `execute_sqlcom_select()`.
- `mariadb/sql/sql_parse.cc:6177-6192` calls
  `open_and_lock_tables(thd, all_tables, ...)`; a tableless `SELECT` therefore
  does not have storage tables to open or lock.
- `mariadb/sql/sql_prepare.cc:5073-5098` prepared execution routes back through
  `mysql_execute_command()`, so direct and prepared tableless `SELECT` share the
  same MariaDB no-table execution semantics.
- MyLite's ownerless direct and prepared paths call
  `refresh_ownerless_external_pages_before_statement()` before executing SQL
  (`packages/libmylite/src/database.cc`). That refresh path is the right place
  for table reads, but it can open or retain page-version visibility state that
  tableless probes do not need.

## Design

Add an ownerless SQL policy helper that recognizes tableless plain reads:

- the first identifier token is `SELECT` or `WITH`,
- no identifier token after the first token is `FROM` or `JOIN`,
- the statement is not a locking read such as `FOR UPDATE` or
  `LOCK IN SHARE MODE`.

For those statements:

- do not allow ownerless page-version reads,
- do not request the global ownerless page refresh path,
- execute the statement through MariaDB normally.

Statements with any `FROM`/`JOIN` token remain on the existing ownerless read
path. This intentionally keeps table reads, subqueries, ordinary CTEs with
table references, view reads, joins, derived tables, DML, DDL, `SHOW`, and
locking reads unchanged.

The helper is conservative. `SELECT ... FROM DUAL` still stays on the existing
path even though it may not touch an InnoDB table, because that refinement is
not required to remove the common PHPUnit probe cost.

## Compatibility Impact

Supported SQL result semantics do not change. MariaDB still parses and executes
the statements. MyLite only avoids ownerless page-version visibility setup when
the statement has no table reference to protect.

## Storage And Recovery Impact

No durable format, directory layout, checkpoint, redo, page-version WAL, or
native recovery behavior changes. The slice removes unnecessary read-visibility
state from tableless probes and does not alter page publication or reclamation.

## Public API Impact

No new public API. The focused regression uses the existing
`mylite_ownerless_pressure_status()` diagnostic to verify tableless reads do
not leave active page-version pins.

## Tests

- Add an ownerless SQL regression proving direct `SELECT 1` leaves
  `active_page_version_pin_count == 0`.
- In the same regression, step a prepared `SELECT 1` result and prove no
  active page-version pin appears while the result cursor is active.
- Keep existing committed external direct/prepared table-read tests as the
  visibility guard for real table reads.
- Run a reduced production embedded performance probe to confirm the `SELECT 1`
  ratio improves while write-path measurements remain separate.

The focused reduced production probe after the slice used
`MYLITE_PERF_SELECT_ITERATIONS=1000` and reported direct ownerless/ordinary
`SELECT 1` ratio `0.7871` and prepared ratio `0.9292`, improved from the
previous reduced sample ratios `0.6658` and `0.6577`.

## Acceptance Criteria

- Direct and prepared ownerless tableless reads do not create page-version pins.
- Direct and prepared ownerless table reads still observe committed peer
  updates.
- Production-build focused tests and static checks pass.
- Compatibility docs record the bounded optimization and remaining ownerless
  performance gaps.
