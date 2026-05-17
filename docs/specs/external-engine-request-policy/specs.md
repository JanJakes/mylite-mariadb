# External Engine Request Policy

## Problem

MyLite routes a small, documented set of requested engines to the MyLite
storage layer. MariaDB also accepts table engine options without an equals sign,
for example `ENGINE FEDERATED`. The first no-equals hardening slice covered
`CSV`, `ARCHIVE`, and `SEQUENCE`, but other bundled optional storage engines
could still reach MariaDB's `enforce_storage_engine=MYLITE` fallback before the
MyLite policy saw the request.

Unsupported external engine requests must fail before MariaDB execution and
before MyLite catalog publication, regardless of whether the DDL uses
`ENGINE=<name>` or MariaDB's `ENGINE <name>` spelling.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5694-5707` parses table options as
  `ENGINE_SYM opt_equal ident_or_text`, stores the parsed value in
  `Storage_engine_name`, and marks `HA_CREATE_USED_ENGINE`.
- `mariadb/sql/sql_yacc.yy:13753-13760` defines `opt_equal` as optional, so
  `ENGINE FEDERATED` and `ENGINE=FEDERATED` are both table-option syntax.
- `mariadb/sql/handler.cc:315-340` resolves storage-engine names and can warn
  instead of erroring when `NO_ENGINE_SUBSTITUTION` is not active.
- `mariadb/sql/sql_table.cc:13342-13412` applies
  `@@enforce_storage_engine`, which can substitute the configured enforced
  handler after an explicit request unless the runtime blocks it earlier.
- Bundled optional MariaDB storage engines include source-tree registrations for
  `ARCHIVE`, `CONNECT`, `EXAMPLE`, `FEDERATED`, `MRG_MyISAM`, `MROONGA`,
  `OQGRAPH`, `PERFORMANCE_SCHEMA`, `ROCKSDB`, `S3`, `SEQUENCE`, `SPHINX`, and
  `SPIDER` under `mariadb/storage/*/CMakeLists.txt` and their plugin
  declarations. The ColumnStore wrapper is also present under
  `mariadb/storage/columnstore/CMakeLists.txt` when its external source tree is
  available.

## Design

Extend MyLite's no-equals engine token recognizer from the already documented
unsupported engines to the bundled optional engine names that the embedded
profile does not route to MyLite semantics. Keep the scanner behavior split:

- `ENGINE=<name>` rejects any unsupported identifier or quoted string;
- `ENGINE <name>` rejects recognized supported or unsupported engine tokens;
- arbitrary unknown no-equals identifiers still fall through to MariaDB parsing
  to avoid treating unrelated top-level tokens after `ENGINE` as policy input.

The supported routing allowlist remains unchanged: `DEFAULT`, `MYLITE`,
`InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and `HEAP`.

## Compatibility Impact

Known external engine names such as `CONNECT`, `FEDERATED`, `MRG_MyISAM`,
`ROCKSDB`, and `S3` now fail with the MyLite-owned unsupported-engine
diagnostic when written as no-equals table options. This prevents silent
substitution into MyLite metadata while keeping the existing routed-engine
behavior unchanged.

## DDL Metadata Routing Impact

Failed external engine requests do not create catalog records and do not mutate
requested/effective engine metadata on existing routed tables.

## Single-File And Embedded-Lifecycle Impact

The change is a pre-execution policy gate. It does not add file-format state or
runtime companions, and it reduces the risk of accidentally entering native
external engine code paths that can own sidecar files, remote connections, or
plugin-specific metadata.

## Public API, File-Format, Build, And Size Impact

No public API, file-format, dependency, or build-profile change is intended.
The binary change is limited to first-party SQL policy code and tests.

## Non-Goals

- Supporting native external storage engines.
- Claiming semantics for remote engines, columnar engines, search engines, or
  native plugin-specific table options.
- Classifying every possible third-party engine name without an equals sign.

## Test Plan

- Direct SQL rejects representative no-equals optional engines:
  `CONNECT`, `FEDERATED`, `MRG_MyISAM`, and `ROCKSDB`.
- Prepared SQL rejects representative no-equals optional engines:
  `CONNECT`, `FEDERATED`, `ROCKSDB`, and `S3`.
- Storage-engine smoke proves representative no-equals external engine requests
  leave the MyLite catalog unchanged.
- Existing routed-engine, sidecar, and server-surface harness groups continue
  to pass.

## Acceptance Criteria

- Known external no-equals engine requests return a MyLite-owned diagnostic
  with no MariaDB errno.
- Failed requests do not create MyLite catalog records.
- Existing supported no-equals routed engine requests still work.
- Compatibility, roadmap, and engine-policy specs describe the broader external
  engine request policy without claiming native external-engine support.
