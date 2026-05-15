# Non-Table Object Policy

## Problem

MyLite routes durable table and schema metadata into the `.mylite` catalog, but
MariaDB still has separate filesystem or server-table paths for non-table
database objects. Allowing those paths to run before MyLite has catalog-backed
object storage would either create runtime-only objects that disappear after
close or persist metadata outside the primary file.

This slice makes the current policy explicit: views, triggers, routines, and
related non-table objects are rejected through the public MyLite SQL entry
points until catalog-backed persistence is designed.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_VIEW`,
  `SQLCOM_DROP_VIEW`, `SQLCOM_CREATE_TRIGGER`, `SQLCOM_DROP_TRIGGER`,
  `SQLCOM_CREATE_PROCEDURE`, `SQLCOM_CREATE_SPFUNCTION`,
  `SQLCOM_DROP_PROCEDURE`, `SQLCOM_DROP_FUNCTION`, and `SQLCOM_CALL` through
  object-specific server paths rather than the MyLite storage handler.
- `mariadb/sql/sql_view.cc::mysql_register_view()` documents and performs view
  registration by writing a view `.frm` definition.
- `mariadb/sql/sql_trigger.cc::mysql_create_or_drop_trigger()` and
  `Table_triggers_list::drop_all_triggers()` operate on trigger files such as
  `.TRG` and `.TRN`.
- `mariadb/sql/sp.cc::Sp_handler::sp_create_routine()` stores routines in
  `mysql.proc` and uses the stored-routine caches.
- `packages/libmylite/src/database.cc` already blocks representative
  server-oriented SQL before calling MariaDB from `mylite_exec()` and
  `mylite_prepare()`.

## Design

- Keep the policy at the `libmylite` SQL boundary for now, matching the current
  server-surface policy implementation.
- Split the existing policy check into:
  - server-oriented SQL surfaces, and
  - unsupported non-table database-object surfaces.
- Reject representative view, trigger, routine, package, sequence, and `CALL`
  statements before MariaDB execution or prepare.
- Return stable MyLite diagnostics with no MariaDB errno, so callers can
  distinguish policy rejection from a MariaDB parse or execution error.
- Keep raw `MYSQL *` adapter enforcement planned; that adapter is not a public
  MyLite API surface yet.

## Affected Subsystems

- `packages/libmylite/src/database.cc` SQL policy gate.
- Embedded direct and prepared `libmylite` tests.
- Storage-engine smoke tests for sidecar protection around blocked non-table
  object DDL.
- Compatibility, API, and roadmap documentation.

## Compatibility Impact

Views, triggers, routines, packages, sequences, and routine invocation remain
unsupported in the current embedded profile. The compatibility status becomes
explicit rejection instead of accidental MariaDB behavior.

This is less compatible than allowing transient MariaDB objects, but it matches
the single-file product invariant and avoids behavior that would silently lose
objects on close or store metadata outside the `.mylite` file.

## DDL Metadata Routing Impact

No new object metadata format is introduced. The slice reserves the future
catalog-backed object work by preventing filesystem-backed publication from
becoming observable behavior.

## Single-File And Embedded Lifecycle

No durable sidecars may be created. Rejected statements must leave the MyLite
runtime cleanup behavior unchanged, and storage-smoke sidecar gates must still
pass after failed non-table object DDL.

## Public API And File Format

No public C API or file-format changes. `mylite_exec()` and `mylite_prepare()`
return `MYLITE_ERROR`, SQLSTATE `HY000`, and stable MyLite diagnostic text for
the newly rejected statements.

## Storage-Engine Routing Impact

Table DDL and DML routing are unchanged. This slice covers object classes that
do not yet have a MyLite storage-engine or catalog-backed path.

## Wire Protocol Or Integration Impact

Future wire-protocol integrations over the MyLite core should inherit this
policy from the public library boundary. A raw MariaDB adapter will need an
equivalent gate before it becomes a supported entry point.

## Binary-Size Impact

No linked component is removed. The runtime change is a small policy predicate
and test coverage.

## License Or Dependency Impact

No dependency is introduced.

## Test And Verification Plan

- Extend embedded direct-SQL tests to reject representative non-table object
  statements with MyLite policy diagnostics.
- Extend embedded prepared-statement diagnostics to reject non-table object
  prepare before MariaDB.
- Extend storage-engine smoke tests to reject non-table object DDL after routed
  schema/table setup and prove durable sidecar gates still pass.
- Run formatting, tidy, dev, embedded, storage-smoke, compatibility report for
  affected groups, and `git diff --check`.

## Acceptance Criteria

- `mylite_exec()` rejects representative view, trigger, routine, package,
  sequence, and `CALL` statements before MariaDB execution.
- `mylite_prepare()` rejects representative non-table object statements before
  MariaDB prepare.
- Rejected non-table object DDL does not create durable files outside the
  primary `.mylite` file.
- Documentation states the policy without claiming catalog-backed object
  support.

## Risks And Unresolved Questions

- The policy tokenizer is representative, not a full SQL parser. It should
  block the common object-entry forms now, and deeper SQL-layer enforcement can
  replace it when raw MariaDB adapter support becomes real.
- Catalog-backed views, triggers, routines, and sequences need a separate
  design for metadata format, dependency tracking, execution, and compatibility
  tests.
