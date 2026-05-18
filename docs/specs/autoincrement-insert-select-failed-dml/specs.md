# Autoincrement INSERT SELECT Failed DML

## Goal

Cover durable first-key `AUTO_INCREMENT` behavior for source-driven
`INSERT ... SELECT` statements on MyLite-routed tables when duplicate rows fail
or are skipped by `INSERT IGNORE`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:4137-4197` prepares `INSERT ... SELECT` through
  `mysql_insert_select_prepare()` and uses the normal insert preparation path
  with `select_insert=true`.
- `mariadb/sql/sql_insert.cc:4226-4448` prepares the `select_insert` result
  handler and calls `ha_start_bulk_insert((ha_rows) 0)` when the selected
  source row count is unknown.
- `mariadb/sql/sql_insert.cc:4476-4529` stores each selected row into the
  insert table, calls `Write_record::write_record()`, and resets the
  autoincrement field before the next selected row.
- `mariadb/sql/sql_insert.cc:2350-2401` handles ordinary insert duplicate
  errors through `Write_record::single_insert()`: non-ignored errors propagate
  through `on_ha_error()`, while `INSERT IGNORE` warns, restores the
  statement-local autoincrement cursor, and skips the duplicate row.
- `mariadb/sql/sql_insert.cc:4708-4748` rolls back `select_insert` result-set
  failures, including partially inserted rows from the same source-driven
  statement.
- `mariadb/sql/handler.cc:4432-4489` treats an insert estimate of `0` as
  unknown and reserves generated values in growing intervals starting with
  `AUTO_INC_DEFAULT_NB_ROWS`.
- `mariadb/sql/handler.cc:4368-4370` defines `AUTO_INC_DEFAULT_NB_ROWS` as
  `1`, so source-driven inserts reserve 1, then 2, then 4 values as selected
  rows outgrow the current interval.
- `mariadb/storage/innobase/handler/ha_innodb.cc:7776-7793` treats duplicate
  errors from `SQLCOM_INSERT_SELECT` as autoincrement-consuming attempts.
- `mariadb/storage/innobase/handler/ha_innodb.cc:16754-16902` advances
  InnoDB's persistent autoincrement state to the reserved interval boundary
  during `get_auto_increment()` under the default lock mode.
- `mariadb/storage/mylite/ha_mylite.cc:1805-1878` publishes durable first-key
  MyLite generated reservations from `get_auto_increment()` and marks them for
  rollback preservation.
- `mariadb/storage/mylite/ha_mylite.cc:1900-2005` calls
  `update_auto_increment()` before duplicate-key checks, so generated source
  rows reserve values before either failure or ignore handling.

## Scope

- Durable MyLite-routed first-key autoincrement tables requested as
  `ENGINE=InnoDB`.
- Plain `INSERT ... SELECT` with a duplicate selected row after an earlier
  successful selected row, proving visible row rollback plus preserved generated
  reservation gaps.
- `INSERT IGNORE ... SELECT` with a duplicate selected row and enough later
  successful selected rows to force unknown-row-count reservation growth.
- Close/reopen persistence for the ignored source-driven reservation tail.

## Non-Goals

- `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE`, covered by a separate slice.
- Grouped later-in-key autoincrement, trigger, view, partition,
  offset/increment, integer-width, or `LAST_INSERT_ID()` matrices.
- Source-read errors unrelated to target duplicate handling.
- Binary log, replication, or wire-protocol behavior.
- Size-profile reduction work.

## Compatibility Impact

This slice makes source-driven generated insert behavior explicit. For failed
plain `INSERT ... SELECT`, MyLite rolls back rows inserted earlier in the
statement while preserving the durable generated reservation boundary requested
before the duplicate failure. For `INSERT IGNORE ... SELECT`, MyLite skips the
duplicate selected row, reuses the statement-local generated cursor for later
successful rows, and preserves the larger unknown-row-count reservation tail
for the next statement.

The claim remains representative. Broader source-driven DML matrices remain
planned for triggers, views, offset/increment coverage, and broader grouped
autoincrement error matrices.

## Design

No production change is expected. The existing MyLite handler already receives
MariaDB's generated reservation requests and statement rollback hooks:

- `ha_mylite::get_auto_increment()` publishes reservation interval boundaries
  before row publication.
- `ha_mylite::write_row()` detects duplicates after generated reservation
  publication.
- MyLite statement checkpoints restore row/index visibility after failed
  source-driven statements while preserving generated autoincrement pages when
  MariaDB has already requested durable reservation semantics.

## File Lifecycle

No file-format change is required. Durable table state remains in the primary
`.mylite` file. Failed statements use existing statement checkpoints, and
close/reopen persistence uses the normal catalog and autoincrement state pages.

## Embedded Lifecycle And API

No `libmylite` API change is required. The behavior is observable through
direct SQL execution. Public insert-id API assertions remain outside this
slice.

## Storage-Engine Routing

The coverage uses requested `ENGINE=InnoDB`, which routes to MyLite storage in
the default embedded profile. Omitted/default, MyISAM, and Aria first-key
tables use the same durable MyLite path but are not repeated here.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for failed ordered `INSERT ... SELECT`
  where a duplicate selected row aborts the statement after an earlier source
  row was inserted.
- Verify failed source-driven insert rollback removes earlier inserted rows and
  that the next generated value resumes after the reserved boundary.
- Add storage-engine smoke coverage for ordered `INSERT IGNORE ... SELECT`
  where a duplicate selected row is skipped and later selected rows continue.
- Verify generated ids assigned after the skipped row, the unknown-row-count
  reserved tail, and close/reopen persistence.
- Run the focused storage-engine test, statement-rollback and transaction
  compatibility harness groups, shell syntax checks, `git diff --check`, and
  the dev, embedded-dev, and storage-smoke presets.

## Acceptance Criteria

- Failed source-driven generated inserts roll back visible rows inserted
  earlier in the same statement.
- Failed source-driven generated inserts preserve durable reservation gaps.
- Ignored source-driven duplicates skip only the duplicate row, allow later
  selected rows to insert, and preserve the unknown-row-count tail gap.
- Roadmap and compatibility docs distinguish covered source-driven generated
  insert behavior from broader source-driven DML matrices.

## Risks And Open Questions

- This slice checks duplicate target failures, not source-read failures.
- The public insert-id API is not asserted here.
- Trigger and view paths may need separate source-backed slices.
  Representative grouped source-driven ODKU source-read errors are covered
  separately in
  `docs/specs/autoincrement-grouped-odku-source-read-errors/specs.md`.
  Representative grouped source-driven ODKU update-expression errors are
  covered separately in
  `docs/specs/autoincrement-grouped-odku-source-driven-update-expression-errors/specs.md`.
