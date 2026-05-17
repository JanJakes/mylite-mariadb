# Unsupported Engine Request Policy

## Problem

The engine-routing policy accepts a small set of MyLite-owned table shapes.
Other explicit `ENGINE=...` requests should not rely on native MariaDB plugin
availability or handler-level fallback errors.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses table options through
  `create_table_option: ENGINE_SYM opt_equal ident_or_text`, so an engine
  request is a table option carried by the parsed `Storage_engine_name`.
- `mariadb/sql/handler.cc` resolves explicit engines through
  `Storage_engine_name::resolve_storage_engine_with_error()` and
  `ha_resolve_by_name()`. Missing or disabled engines can therefore vary by
  plugin registration and SQL mode unless MyLite rejects unsupported shapes
  before MariaDB execution.
- `mariadb/sql/handler.h` `HA_CREATE_INFO::use_default_db_type()` routes omitted
  engines to the session default handler. MyLite keeps that path for omitted
  engines and for the supported routed aliases.

## Design

- Generalize the table-option scanner used for CSV so `CREATE TABLE` and
  `ALTER TABLE` requests with explicit unsupported `ENGINE=...` values fail
  before MariaDB execution.
- Limit the scanner to top-level table options so quoted text and nested
  expressions or constraints containing `ENGINE=...` are not treated as engine
  requests.
- Keep the accepted engine list aligned with current routed behavior:
  `DEFAULT`, `MYLITE`, `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and
  `HEAP`.
- Recognize identifier engine names and quoted text engine names after an
  explicit `=`. The scanner is intentionally a MyLite policy gate, not a full
  replacement for MariaDB parsing.
- Preserve the specific CSV diagnostic because CSV was removed as a native
  file-backed engine and has dedicated documentation.

## Non-Goals

- This slice does not add support for native external engines.
- This slice does not claim full SQL grammar coverage for every MariaDB table
  option spelling; unsupported forms outside the policy scanner remain future
  compatibility-hardening work.
- This slice does not change routed storage semantics for `InnoDB`, `MyISAM`,
  `Aria`, `BLACKHOLE`, `MEMORY`, `HEAP`, omitted engines, or `MYLITE`.

## Compatibility Impact

Unsupported explicit engines such as `ARCHIVE` now fail with a stable MyLite
diagnostic and no MariaDB errno before catalog publication. Supported routed
engines continue through the existing MariaDB parse and MyLite handler path.

## DDL Metadata Routing Impact

Failed unsupported engine requests do not publish MyLite catalog records and do
not mutate requested/effective engine metadata on existing routed tables.

## Single-File And Embedded-Lifecycle Impact

Rejecting unsupported engines before MariaDB execution prevents fallback into
native plugin behavior that could create external durable files. The policy is
handle-local diagnostic behavior and does not add process-global state.

## Public API, File Format, And Binary-Size Impact

The public C API, `.mylite` file format, and embedded binary profile are
unchanged. The policy only changes which error is returned for unsupported
explicit table-engine requests.

## Test Plan

- Direct and prepared SQL reject `ENGINE=ARCHIVE` creates, quoted
  `ENGINE='ARCHIVE'` creates, and engine changes.
- Follow-up no-equals policy coverage rejects representative bundled optional
  engine names such as `ENGINE CONNECT`, `ENGINE FEDERATED`, `ENGINE ROCKSDB`,
  and `ENGINE S3` before MariaDB execution.
- Storage-engine smoke verifies unsupported engine requests do not create a
  catalog record or mutate an existing routed table's requested/effective
  engine metadata.
- Existing CSV, routed engine, sidecar, and server-surface coverage continues
  to pass.

## Acceptance Criteria

- Unsupported explicit engine requests return a MyLite-owned diagnostic and no
  MariaDB errno.
- CSV continues to return the CSV-specific diagnostic.
- Failed unsupported engine requests leave MyLite catalog metadata unchanged.
- Compatibility and storage-engine harness groups include the behavior.
