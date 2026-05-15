# SQL File I/O Policy

## Problem

MariaDB exposes host filesystem I/O through SQL surfaces that do not fit
MyLite's core embedded, file-owned API:

- `LOAD_FILE()` reads a server-side file into a result expression.
- `SELECT ... INTO OUTFILE` writes a text export file.
- `SELECT ... INTO DUMPFILE` writes a binary dump file.

These surfaces bypass normal table storage and create or read files outside the
documented `.mylite` file lifecycle. MyLite should reject them explicitly at the
public SQL policy boundary before MariaDB execution can touch host files.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/item_create.cc:1675-1688` defines
  `Create_func_load_file`.
- `mariadb/sql/item_create.cc:4980-4989` marks `LOAD_FILE()` unsafe and
  returns an `Item_load_file`.
- `mariadb/sql/item_create.cc:6477` registers `LOAD_FILE` as a native SQL
  function.
- `mariadb/sql/item_strfunc.cc:4387-4457` implements
  `Item_load_file::val_str()` with host path formatting, `secure_file_priv`
  checks, file stat, file open, and file read calls.
- `mariadb/sql/sql_yacc.yy:13358-13377` parses `INTO OUTFILE` and
  `INTO DUMPFILE` by creating `select_export` or `select_dump` result
  interceptors.
- `mariadb/sql/sql_parse.cc:3993-4006` treats `lex->exchange != NULL` as
  `SELECT ... INTO OUTFILE` and requests `FILE_ACL`.
- `mariadb/sql/sql_parse.cc:6150-6165` documents that ordinary `EXPLAIN`
  should return rows even for a query that would otherwise redirect output.
- `mariadb/sql/sql_class.cc:3420-3456` creates export files for select-to-file
  paths and enforces `secure_file_priv`.
- `mariadb/sql/sql_class.cc:3464-3615` prepares and writes text rows for
  `select_export`.
- `mariadb/sql/sql_class.cc:3814-3851` prepares and writes binary rows for
  `select_dump`.
- `docs/architecture/bundle-size-research.md` records prior successful trims
  for `LOAD_FILE()` utility-function support and for `SELECT ... INTO OUTFILE`
  / `DUMPFILE` writer bodies.

## Scope

- Reject direct and prepared SQL that invokes `LOAD_FILE()`.
- Reject direct and prepared `SELECT ... INTO OUTFILE`.
- Reject direct and prepared `SELECT ... INTO DUMPFILE`.
- Preserve ordinary `SELECT` result delivery.
- Preserve `SELECT ... INTO` user-variable assignment.
- Update server-surface compatibility documentation and harness wording.

## Non-Goals

- Remove SQL grammar for OUTFILE or DUMPFILE.
- Remove `LOAD_FILE()` builders or file-writer bodies from MariaDB source in
  this policy slice.
- Add a replacement MyLite-managed import/export API.
- Change `LOAD DATA` / `LOAD XML` behavior; that remains covered by the file
  import policy slice.
- Change `EXPLAIN SELECT ... INTO OUTFILE` behavior.

## Design

Extend the existing MyLite SQL policy gate in `database.cc`:

- keep `LOAD DATA` / `LOAD XML` in the existing file-import detector;
- add `LOAD_FILE` function-call detection by scanning SQL tokens outside
  strings, quoted identifiers, and ordinary comments, and requiring an opening
  parenthesis after the token to avoid obvious identifier false positives;
- add a file-export detector for statements that contain `INTO OUTFILE` or
  `INTO DUMPFILE` token pairs outside strings, quoted identifiers, and
  ordinary comments, while scanning MariaDB executable comments and leaving
  ordinary `EXPLAIN` to MariaDB because it does not write the file in the
  source-documented server flow.

The gate returns stable MyLite errors before direct execution or prepared
statement preparation calls into MariaDB.

## Compatibility Impact

This makes unsupported host-file SQL explicit. Applications should use ordinary
SQL result fetching for exports, ordinary `INSERT` / prepared DML / CTAS for
ingestion, or a future MyLite-managed file I/O API if one is designed.

`SELECT ... INTO @variable` remains supported because it stores session state,
not host files.

## Single-File And Embedded-Lifecycle Impact

The slice strengthens the single-file contract by preventing SQL from reading
or writing application-visible files outside the `.mylite` lifecycle. It does
not add new durable files or companions.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

None. Rejected SQL never reaches handler write paths.

## Wire-Protocol Or Integration-Package Impact

Core `libmylite` remains file-owned and embedded-only. A future wire-protocol
adapter that wants MySQL/MariaDB file import/export compatibility must design a
controlled adapter-level surface rather than relying on host-file SQL inside
the core library.

## Binary-Size Impact

No default profile change in this policy slice. The follow-up size slice can
compile out `LOAD_FILE()` and select-to-file writer bodies using this policy as
the public behavior gate.

## Test And Verification Plan

- Add direct `mylite_exec()` tests for `LOAD_FILE()`, `SELECT ... INTO
  OUTFILE`, and `SELECT ... INTO DUMPFILE` rejection.
- Add prepared `mylite_prepare()` tests for the same rejected surfaces.
- Add non-regression coverage for quoted text containing these tokens.
- Add direct coverage for MariaDB executable comments that contain file-I/O
  SQL.
- Add non-regression coverage for `SELECT ... INTO @variable`.
- Run embedded exec and statement tests.
- Run the `server-surface` compatibility harness group.
- Run formatting, shell syntax, whitespace, and first-party tidy checks.

## Acceptance Criteria

- Direct and prepared file SQL fails with stable MyLite diagnostics and no
  MariaDB errno.
- Ordinary SELECT text containing file-I/O keywords still succeeds.
- `SELECT ... INTO @variable` still succeeds.
- Compatibility and API docs describe SQL file I/O as out of scope for the
  embedded core.
- The follow-up size trim has clear source references and public behavior
  evidence.

## Risks And Open Questions

- The policy gate is a targeted scanner, not a full SQL parser. It is suitable
  for explicit unsupported-surface rejection but should stay conservative.
- `EXPLAIN SELECT ... INTO OUTFILE` is intentionally left to MariaDB because it
  does not write the file in the ordinary server flow documented by source
  comments.
- A future adapter-level export/import API needs separate lifecycle, security,
  and compatibility design.
