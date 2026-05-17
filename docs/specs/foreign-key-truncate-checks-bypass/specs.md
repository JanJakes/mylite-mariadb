# Foreign-Key Truncate Checks Bypass

## Goal

Honor session `foreign_key_checks=0` for MyLite's parent-table
`TRUNCATE TABLE` checks.

MyLite already honors the session option for supported FK row DML. After the
handlerton advertises MyLite's covered FK subset, MariaDB's truncate path uses
the same handler FK metadata hooks to reject truncating referenced parent
tables while checks are enabled. This slice covers the matching MariaDB behavior
when checks are disabled: parent truncate may proceed and re-enabling checks
does not retroactively validate orphan child rows.

## Non-Goals

- Changing child/parent row-DML bypass behavior.
- Retrospective validation when `foreign_key_checks` is set back to `1`.
- Cascades, `SET NULL`, `SET DEFAULT`, deferrable constraints, or full InnoDB
  transaction semantics.
- Allowing unsupported FK DDL shapes, temporary-table FKs, volatile FK tables,
  or row-discarding FK tables.
- Implementing SQL rollback for `TRUNCATE TABLE`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_foreign_key_checks` stores
  `foreign_key_checks` in `THD::variables.option_bits` through the reverse
  `OPTION_NO_FOREIGN_KEY_CHECKS` bit.
- `mariadb/sql/sql_truncate.cc:mysql_truncate()` checks
  `!(thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS)` before calling
  `fk_truncate_illegal_if_parent()`.
- `mariadb/sql/sql_truncate.cc:fk_truncate_illegal_if_parent()` asks the
  handler whether the table is referenced by an FK and then uses
  `get_parent_foreign_key_list()` to allow only self-referencing parent FKs.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::truncate()` performs the
  physical MyLite logical-truncate operation. It should not duplicate the
  SQL-layer FK option check; the SQL layer owns whether the handler truncate is
  allowed to run.
- `docs/specs/foreign-key-checks-bypass/specs.md` covers row-DML bypass only
  and explicitly left broader DDL effects out of scope.

## Compatibility Impact

This matches the MariaDB session-option shape for parent-table truncation under
the supported MyLite FK subset:

- with checks enabled, truncating a referenced non-self parent table remains an
  error;
- with checks disabled, truncating that parent table succeeds and may leave
  orphan child rows;
- after checks are re-enabled, subsequent FK row checks are enforced, but
  existing orphan rows are not revalidated.

Full FK action support remains partial. Cascades, `SET NULL`, `SET DEFAULT`,
and transactional truncate rollback remain separate roadmap work.

## Design

No handler code change should be required. The MyLite handler must keep
returning parent FK metadata through `referenced_by_foreign_key()` and
`get_parent_foreign_key_list()`. MariaDB's SQL layer then decides whether to
call `fk_truncate_illegal_if_parent()` based on `foreign_key_checks`.

Add storage-smoke coverage around public FK DDL:

1. Create a parent and child table with a supported `RESTRICT` / `NO ACTION`
   FK.
2. Insert a referenced parent row and child row.
3. Verify `TRUNCATE TABLE parent` fails while checks are enabled.
4. Set `foreign_key_checks=0` and verify `TRUNCATE TABLE parent` succeeds.
5. Re-enable checks, verify the orphan child row remains, and verify a new
   missing-parent child insert fails.
6. Reopen the file and verify checks default back on while the orphan row and
   truncated parent state persist.

## Single-File And Embedded Lifecycle

No file-format change is required. Truncation uses the existing logical
row-state and autoincrement reset path in the primary `.mylite` file. No new
sidecar is introduced. The bypass is session state only and must not persist in
the `.mylite` file.

## Storage-Engine Routing Impact

The behavior applies only to durable MyLite-routed base tables in the supported
FK subset, including omitted-engine and routed `ENGINE=InnoDB` tables. Volatile
and row-discarding FK tables remain unsupported at FK DDL validation time.

## Public API, Wire-Protocol, Build, And Dependency Impact

No public C API, wire-protocol, dependency, or binary-size change is expected.
The coverage runs through existing direct SQL execution.

## Test And Verification Plan

- Add storage-smoke direct SQL coverage in
  `packages/libmylite/tests/embedded_storage_engine_test.c`.
- Run the MariaDB MyLite storage archive build.
- Run embedded storage/exec/statement smoke tests.
- Run default first-party format and storage checks.
- Run `git diff --check`.

## Acceptance Criteria

- `TRUNCATE TABLE` of a referenced non-self parent fails with
  `foreign_key_checks=1`.
- The same truncate succeeds with `foreign_key_checks=0`.
- Re-enabling checks does not revalidate orphan rows.
- Subsequent child inserts still enforce FK checks after re-enabling.
- Close/reopen resets the session option to checks enabled while preserving the
  file state.

## Risks And Open Questions

- This intentionally allows orphan rows when checks are disabled. That is
  compatible with the documented session option but must remain explicit in the
  compatibility matrix.
- SQL rollback for truncate remains unsupported, so this slice must not claim
  transactional truncate semantics.
