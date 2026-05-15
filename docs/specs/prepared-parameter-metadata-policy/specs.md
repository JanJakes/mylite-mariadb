# Prepared Parameter Metadata Policy

## Goal

Clarify MyLite's prepared-statement parameter metadata boundary.

MyLite already exposes parameter marker counts through
`mylite_bind_parameter_count()`. Rich parameter metadata such as inferred
server-side parameter names, SQL types, flags, charsets, and lengths should not
be exposed until MyLite owns a real design for it, because MariaDB 11.8 does not
provide this metadata through the prepared-statement API.

## Non-Goals

- Do not add new public `libmylite` API in this slice.
- Do not expose raw `MYSQL_STMT *`, `MYSQL_RES *`, or `MYSQL_FIELD *` handles.
- Do not infer parameter types from current bound values and present them as
  SQL parameter metadata.
- Do not implement parser-derived parameter metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c` reads the prepared response
  packet into `stmt->param_count` in `mthd_stmt_read_prepare_response()`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c` implements
  `mysql_stmt_param_count()` by returning `stmt->param_count`.
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c` implements
  `mysql_stmt_param_metadata()` as an unconditional `NULL` return with the
  comment that the server does not deliver parameter information yet.
- `mariadb/libmariadb/man/mysql_stmt_param_metadata.3` documents
  `mysql_stmt_param_metadata()` as not implemented and reserved for future use;
  its return value is always `NULL`.
- `mariadb/libmariadb/man/mysql_stmt_param_count.3` documents parameter marker
  counts as the supported prepared-statement parameter introspection surface.

## Design

Keep the current public surface:

- `mylite_bind_parameter_count()` returns the prepared statement's parameter
  marker count;
- binding functions remain the source of caller-provided value types for a
  specific execution;
- result-column metadata remains exposed through the existing
  `mylite_column_*()` accessors.

Do not add a `mylite_parameter_*()` metadata family until one of these becomes
true:

- MariaDB exposes authoritative parameter metadata for the selected base line;
- MyLite adds parser-derived metadata with clear type, collation, and
  expression-resolution rules;
- a compatibility adapter has a concrete application requirement and can state
  its fallback behavior when metadata is unavailable.

## Compatibility Impact

This keeps MyLite aligned with MariaDB's observable prepared-statement API:
parameter counts are available, while rich parameter metadata is explicitly
unsupported instead of accidentally absent.

Applications that need to know how many parameters to bind can use
`mylite_bind_parameter_count()`. Applications that expect MySQL/MariaDB
parameter metadata beyond the count must treat it as unavailable for now.

## Single-File And Storage Impact

No file-format, storage, catalog, sidecar, or recovery behavior changes.

## Embedded Lifecycle And API

No handle ownership or lifecycle changes. The existing parameter count is
statement-owned and available after successful `mylite_prepare()`.

## Build, Size, And Dependencies

No build-profile, dependency, or binary-size impact.

## Test Plan

Existing embedded prepared-statement coverage already asserts representative
parameter counts through `mylite_bind_parameter_count()`. This slice updates
the design and compatibility documentation only.

## Acceptance Criteria

- API and compatibility docs describe parameter counts as implemented.
- API and compatibility docs describe rich parameter metadata as deliberately
  unsupported on the current MariaDB base.
- The roadmap no longer implies that MariaDB-backed rich parameter metadata is
  an ordinary near-term implementation gap.

## Risks And Open Questions

- MyLite may eventually need parser-derived parameter metadata for some
  adapters. That should be a separate design because the metadata would be a
  MyLite interpretation, not a direct MariaDB API result.
