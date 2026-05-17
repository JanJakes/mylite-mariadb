# Engine Option Syntax Hardening

## Problem

MariaDB accepts table engine options with and without an equals sign. MyLite's
unsupported-engine policy should cover the common no-equals spelling for known
engine names without treating arbitrary column or expression tokens as table
engine options.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` defines
  `create_table_option: ENGINE_SYM opt_equal ident_or_text`, so the `=` between
  `ENGINE` and the engine name is optional for table options.
- `mariadb/sql/sql_yacc.yy` uses the same `create_table_option` production for
  `CREATE TABLE` table options and `ALTER TABLE` table options.
- MyLite's policy scanner runs before MariaDB execution, so it must stay narrow
  enough to avoid rejecting ordinary SQL that contains the token `ENGINE` in a
  quoted string, a comment, or a nested expression.

## Design

- Extend the top-level table-option scanner to accept `ENGINE <name>` as well
  as `ENGINE=<name>`.
- Require no-equals engine names to be recognized engine-name tokens. This
  covers routed engines plus the known unsupported `CSV`, `ARCHIVE`, and
  `SEQUENCE` names MyLite already documents; the follow-up external-engine
  request policy broadens this to additional bundled optional engine names
  while avoiding broad false positives after arbitrary identifiers.
- Continue to parse quoted engine names for the explicit-equals form.
- Keep unsupported `CSV` requests on the existing CSV-specific diagnostic.

## Compatibility Impact

`CREATE TABLE ... ENGINE ARCHIVE`, `ALTER TABLE ... ENGINE ARCHIVE`, and
`CREATE TABLE ... ENGINE CSV` now fail through the same MyLite-owned policy as
the equals forms. Supported routed no-equals requests such as
`ENGINE InnoDB` continue into MariaDB and MyLite routing.

## DDL Metadata Routing Impact

Failed no-equals unsupported engine requests do not publish MyLite catalog
records and do not mutate existing requested/effective engine metadata.

## Non-Goals

- This slice does not implement native external engines.
- This slice does not attempt to classify every possible unknown no-equals
  engine token before MariaDB parsing. Unknown `ENGINE=<name>` remains covered.

## Test Plan

- Direct and prepared SQL reject no-equals `ENGINE ARCHIVE`.
- Direct SQL rejects no-equals `ENGINE CSV` with the CSV-specific diagnostic.
- Storage-engine smoke proves `ENGINE InnoDB` remains routed and no-equals
  unsupported requests leave catalog metadata unchanged.
- Existing unsupported engine, sidecar, and server-surface groups continue to
  pass.

## Acceptance Criteria

- No-equals known unsupported engine requests return MyLite-owned diagnostics
  with no MariaDB errno.
- Routed no-equals `InnoDB` metadata stores requested engine `InnoDB` and
  effective engine `MYLITE`.
- Quoted/commented text containing engine-option spellings does not trigger the
  policy.
