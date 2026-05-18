# Foreign-Key Checks Bypass

## Goal

Honor session `foreign_key_checks=0` for MyLite's supported foreign-key row
checks.

When checks are disabled, MyLite should allow child rows whose parents do not
yet exist and parent row updates/deletes that would otherwise be blocked by a
supported `RESTRICT` / `NO ACTION` constraint. Re-enabling checks must affect
subsequent row DML only; it must not retrospectively validate rows written while
checks were disabled.

## Non-Goals

- Disabling FK DDL shape validation or metadata publication checks.
- Retrospective validation when `foreign_key_checks` is set back to `1`.
- Cascades, `SET NULL`, `SET DEFAULT`, deferrable constraints, or full InnoDB
  transaction semantics.
- Automatically repairing or reporting orphan rows introduced while checks were
  disabled.
- Changing `DROP TABLE`, `TRUNCATE`, or broader DDL behavior beyond row DML.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_foreign_key_checks` stores
  `foreign_key_checks` in `THD::variables.option_bits` with the reverse
  `OPTION_NO_FOREIGN_KEY_CHECKS` bit and documents out-of-order reloads plus no
  retrospective consistency check when re-enabled.
- `mariadb/sql/sql_priv.h` defines `OPTION_NO_FOREIGN_KEY_CHECKS` as a THD,
  user, and binlog option used to suppress FK checks.
- `mariadb/sql/sql_truncate.cc:mysql_truncate()` skips parent-FK truncate
  checks when `OPTION_NO_FOREIGN_KEY_CHECKS` is set.
- `mariadb/sql/sql_table.cc:fk_check_column_changes()` and surrounding ALTER
  checks allow ALTER paths that would delete parent rows when
  `OPTION_NO_FOREIGN_KEY_CHECKS` is set.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_trx_init()` reads
  `OPTION_NO_FOREIGN_KEY_CHECKS` from the user `THD` with
  `thd_test_options()` before initializing InnoDB FK-check state.
- Before this slice,
  `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`,
  `update_row()`, and `delete_row()` called MyLite child and parent FK row
  checks for durable routed tables without consulting
  `OPTION_NO_FOREIGN_KEY_CHECKS`.

## Compatibility Impact

This brings MyLite's supported FK subset closer to MySQL/MariaDB dump-import
behavior: applications may disable checks, import child rows before parent
rows, and re-enable checks without existing orphan rows being revalidated.

Compatibility remains partial. Unsupported FK actions, temporary-table FK DDL,
generated supporting-key cleanup, handlerton FK advertising, and parent-table
truncate bypass remain out of scope for this row-DML bypass slice. Parent
truncate behavior is covered by
[Foreign-Key Truncate Checks Bypass](../foreign-key-truncate-checks-bypass/specs.md).

## Design

1. Add a small handler-side predicate that returns true when the row handler's
   owning `THD` has `OPTION_NO_FOREIGN_KEY_CHECKS`.
2. In `ha_mylite::write_row()`, skip `mylite_check_child_foreign_keys()` when
   rows are durable and checks are disabled.
3. In `ha_mylite::update_row()`, skip both child and parent FK row checks when
   rows are durable and checks are disabled.
4. In `ha_mylite::delete_row()`, skip parent FK row checks when rows are
   durable and checks are disabled.
5. Keep FK DDL validation unchanged: table shape, action, match, parent engine,
   child supporting key, and parent unique-key checks still run regardless of
   `foreign_key_checks`.
6. Leave parent-table `TRUNCATE` behavior to a follow-up DDL slice.

## File Lifecycle

No file-format or companion-file change is required. Rows and index entries
written while checks are disabled use the same primary-file pages as ordinary
row DML. Existing rollback-journal and statement/transaction checkpoints
continue to control publication and rollback.

## Embedded Lifecycle And API

Direct and prepared `SET foreign_key_checks=0/1` are already accepted as
ordinary MariaDB session variable assignments in the storage-smoke profile.
The bypass applies to subsequent direct and prepared row DML on the same
session handle and persists only as session state, not in the `.mylite` file.

## Build, Size, And Dependencies

No dependency or size-profile change is expected.

## Test Plan

- Storage-smoke direct SQL coverage:
  - missing-parent child insert succeeds with checks disabled,
  - parent update/delete succeeds with checks disabled,
  - re-enabling checks does not revalidate orphan rows,
  - subsequent missing-parent child inserts fail again.
- Storage-smoke prepared coverage for at least one row-DML bypass path.
- Close/reopen coverage proves orphan rows written while disabled remain
  ordinary durable rows and checks are back to the default enabled session
  behavior on a new handle.
- A follow-up dump fixture covers child-row import before parent-row import
  under the same session bypass.
- Verification: storage-smoke embedded tests, default storage tests,
  format-check, and `git diff --check`.

## Acceptance Criteria

- MyLite skips supported FK child and parent row checks only while
  `foreign_key_checks=0` is active on the current session.
- Re-enabling checks affects future DML and does not retroactively scan or
  reject existing rows.
- FK metadata shape validation and unsupported FK actions remain unchanged.
- Docs and compatibility matrix distinguish row-check bypass from full FK
  compatibility.
