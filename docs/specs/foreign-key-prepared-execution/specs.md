# Foreign-Key Prepared Execution

## Goal

Prove that the supported public foreign-key subset works through
`libmylite` prepared execution, not only direct SQL execution. The slice covers
prepared FK DDL publication, prepared child and parent row-check diagnostics,
and prepared success paths for the same durable MyLite-routed table shapes that
direct execution already covers.

## Non-Goals

- Prepared `START TRANSACTION`, `COMMIT`, or `ROLLBACK` support.
- Parameter metadata enhancements beyond the current MariaDB base.
- Cascades, `SET NULL`, `SET DEFAULT`, deferrable checks, or multi-row FK
  ordering.
- Temporary, volatile, row-discarding, partitioned, or unsupported FK table
  shapes.
- Wire-protocol integration beyond the embedded `libmylite` prepared API.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/libmysqld/libmysql.c:mysql_stmt_prepare()` prepares embedded
  statements through the MariaDB client/server statement path used by
  `libmylite`.
- `mariadb/libmysqld/libmysql.c:mysql_stmt_execute()` executes a prepared
  statement and reports server/handler errors through statement diagnostics.
- `packages/libmylite/src/database.cc:mylite_prepare()` calls
  `mysql_stmt_prepare()`, captures MariaDB diagnostics and warnings on prepare
  failure, records whether the statement needs storage checkpoints, and stores
  parameter metadata.
- `packages/libmylite/src/database.cc:mylite_step()` binds parameters, wraps
  storage-mutating statements in the current statement checkpoint path, calls
  `mysql_stmt_execute()`, and rolls back the checkpoint before capturing
  MariaDB diagnostics and warnings when execution fails.
- Current storage-smoke coverage already exercises some prepared DDL and DML
  statements around FKs, but it does not explicitly prove prepared FK creation,
  prepared child-row violation diagnostics, prepared parent-row violation
  diagnostics, and prepared successful row checks as one public API surface.

## Compatibility Impact

The supported FK subset should explicitly include prepared execution for:

- `CREATE TABLE` FK publication in the supported `RESTRICT` / `NO ACTION`
  shape;
- successful child inserts referencing existing parent rows;
- child insert/update failures when the parent key is missing;
- parent update/delete failures when live child rows reference the old parent
  key.

Unsupported FK shapes and broader FK action semantics remain unchanged.

## Design

No production code change is expected. The prepared API already reaches the
same MariaDB SQL and MyLite handler paths as direct execution. Add focused
storage-smoke coverage that:

1. Creates parent and child tables through prepared statements.
2. Executes prepared child inserts that both satisfy and violate the FK.
3. Executes prepared parent updates/deletes that fail with FK diagnostics.
4. Verifies successful prepared DML still leaves durable rows visible after
   close/reopen through the existing FK smoke database.

Use existing test helpers for prepared zero-parameter DDL/DML and prepared
failure diagnostics. Parameterized FK DML can be covered later with a broader
prepared-parameter matrix.

## Single-File And Embedded Lifecycle

No file-format or file-lifecycle change is required. The coverage must prove
prepared FK metadata and successful prepared rows persist in the primary
`.mylite` file across close/reopen.

## Public API, Storage Routing, Build, And Dependency Impact

The public API surface is unchanged. This slice documents and tests behavior
through existing `mylite_prepare()` and `mylite_step()` calls. No new
dependency or binary-size impact is expected.

## Test And Verification Plan

- Add storage-smoke prepared FK DDL/DML coverage in
  `packages/libmylite/tests/embedded_storage_engine_test.c`.
- Run embedded storage/exec/statement smoke tests.
- Run default first-party format and storage checks.
- Run `git diff --check`.

## Acceptance Criteria

- Prepared `CREATE TABLE` can publish a supported FK definition.
- Prepared child inserts succeed when the parent exists and fail when missing.
- Prepared parent update/delete statements fail when live children reference
  the old parent key.
- Prepared FK-created metadata and successful rows remain visible after
  close/reopen.
- Compatibility and API docs state prepared execution coverage for the
  supported FK subset without expanding unsupported FK actions.

## Risks And Open Questions

- Parameterized FK statements are not covered by this slice; they should be
  grouped with broader prepared binding matrices rather than hidden in this
  zero-parameter coverage.
- Prepared transaction-control statements remain intentionally unsupported, so
  this does not claim prepared multi-statement transaction orchestration.
