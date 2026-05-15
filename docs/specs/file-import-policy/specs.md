# File Import Policy

## Problem

MariaDB implements `LOAD DATA` and `LOAD XML` as server/client file-import
statements. MyLite's core API is an embedded, file-owned library centered on a
single primary `.mylite` file. Letting those statements reach MariaDB would add
uncontrolled filesystem and client-protocol behavior before MyLite has a
designed bulk-import surface, and it would weaken size-profile work that marks
the upstream LOAD execution object as removable.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:15253-15278` parses `LOAD DATA` and `LOAD XML`
  through the same `SQLCOM_LOAD` command, including `LOCAL` and `INFILE`
  variants.
- `mariadb/sql/sql_parse.cc:549-551` marks `SQLCOM_LOAD` as data-changing,
  re-execution-fragile, progress-reporting, and row-event-generating.
- `mariadb/sql/sql_parse.cc:4900-4924` validates `LOAD DATA LOCAL` client
  capability and server file privileges before calling `mysql_load()`.
- `mariadb/sql/sql_load.cc:330-354` documents `mysql_load()` as the execution
  path for server file and client file reads.
- `mariadb/sql/sql_load.cc:385-387` forces `read_file_from_client = 0` for
  embedded builds because the server is in the same process.
- `mariadb/sql/sql_prepare.cc:2260-2264` routes prepared `SQLCOM_LOAD`
  validation through the insert-common preparation path.
- `docs/architecture/bundle-size-research.md` records upstream LOAD execution
  removal as a size-profile candidate and notes that server-file import is
  unsupported.

## Scope

- Reject `LOAD DATA` and `LOAD XML` through `mylite_exec()` before MariaDB
  execution.
- Reject prepared `LOAD DATA` and `LOAD XML` through `mylite_prepare()` before
  MariaDB prepare/validation.
- Return a stable MyLite error with SQLSTATE `HY000` and no MariaDB errno, like
  the other explicit unsupported SQL surface gates.
- Preserve ordinary `INSERT`, prepared DML, `CREATE TABLE ... SELECT`, and
  quoted text containing LOAD tokens.

## Non-Goals

- Implement bulk file import.
- Implement network-protocol `LOAD DATA LOCAL`.
- Implement a MyLite-specific import API.
- Change MariaDB parser grammar or remove the upstream LOAD code in this slice.
- Add statement-rollback coverage for LOAD, because LOAD is not an accepted
  execution path in the current core API.

## Design

Add a first-party SQL surface check beside the existing server, non-table
object, transaction, locking, partition, online ALTER, and foreign-key policy
checks. The check scans SQL tokens, ignores comments and quoted spans, and only
rejects statements whose first token is `LOAD` and second token is `DATA` or
`XML`.

The rejection message is `unsupported SQL file import surface`. Keeping this as
a MyLite-owned error avoids depending on MariaDB file path, privilege,
embedded-client, or target-table diagnostics.

## Compatibility Impact

`LOAD DATA` and `LOAD XML` are deliberately out of scope for the core embedded
API until MyLite has a controlled import design. Applications can still ingest
data through ordinary `INSERT`, prepared statements, and supported CTAS paths.
Future wire-protocol or adapter packages can design a compatible import story
without requiring the core file-owned library to expose server/client file
reads.

## Single-File And Embedded-Lifecycle Impact

The slice does not change file format or storage publication. It prevents
accepted SQL from reading external files or depending on protocol-local data
streams before that behavior is specified. It also avoids new temporary files
or durable sidecars.

## Public API And File-Format Impact

No public C API or file-format changes are required. The public behavior change
is a documented stable rejection for direct and prepared LOAD statements.

## Storage-Engine Routing Impact

None. The statement is rejected before reaching handler open/write paths.

## Wire-Protocol Or Integration-Package Impact

The core API rejects `LOAD DATA LOCAL`. A later wire-protocol package may choose
to translate client-file imports into a MyLite-owned bulk import mechanism, but
that is separate from this slice.

## Binary-Size And Dependency Impact

No dependency is added. This slice creates the compatibility precondition for a
later size-profile trim that removes unused LOAD execution code from the
embedded profile.

## Test And Verification Plan

- Add direct SQL coverage for `LOAD DATA INFILE`, `LOAD DATA LOCAL INFILE`, and
  `LOAD XML INFILE` rejection.
- Add prepared statement coverage for `LOAD DATA` and `LOAD XML` rejection.
- Assert quoted LOAD text remains executable.
- Run the embedded exec/statement tests and the `server-surface`
  compatibility-harness group.
- Run formatting, shell syntax, whitespace, and tidy checks.

## Acceptance Criteria

- Direct and prepared LOAD statements fail before MariaDB execution with the
  documented MyLite error.
- Rejection diagnostics are stable and do not depend on filesystem paths,
  privileges, client protocol state, or target-table existence.
- Existing INSERT, prepared DML, and CTAS behavior is unchanged.
- Compatibility, API, roadmap, and harness docs describe LOAD file import as an
  explicit unsupported surface.
