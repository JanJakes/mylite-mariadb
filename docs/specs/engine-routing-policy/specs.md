# Engine Routing Policy

## Problem Statement

MyLite can store and rediscover explicit `ENGINE=MYLITE` table-definition
metadata, but application DDL usually asks for no engine, `InnoDB`, `MyISAM`,
`Aria`, or selected zero-file engines. The next storage slice needs those
requests to resolve to the MyLite handler without reviving MariaDB engine
sidecars, while still preserving which engine the application requested.

This slice implements the first metadata-only routing policy for file-backed
embedded MyLite runtimes.

## Scope

- Route no-engine/default-engine DDL to MyLite for file-backed `libmylite`
  opens when the static MyLite storage engine is present.
- Route explicit `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`,
  `ENGINE=Aria`, and follow-up `ENGINE=BLACKHOLE` table creation to the MyLite
  handler.
- Record the requested engine name separately from the effective MyLite engine
  in the MyLite catalog.
- Reject unsupported explicit engine requests after MariaDB routes them to the
  MyLite handler, before publishing catalog metadata.
- Keep row DML, `DROP`, `RENAME`, `ALTER`, partitioning, and real InnoDB
  foreign-key semantics outside the first routing slice; later slices cover
  durable rows and BLACKHOLE row discard.
- Keep `:memory:` opens outside this routing policy because they do not have a
  primary MyLite file for durable catalog publication.

## Non-Goals

- Do not implement row storage or index storage.
- Do not claim full InnoDB, MyISAM, Aria, or native BLACKHOLE semantics. This
  slice only routes table definitions where MyLite can safely persist the
  definition, with BLACKHOLE row-discard behavior covered by a later slice.
- Do not enable ordinary MariaDB durable sidecars as compatibility fallbacks.
- Do not add a server daemon policy or wire-protocol behavior.
- Do not make dynamic external engines user-selectable in the core embedded
  profile.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_cmd.h:29-43` stores the original `ENGINE=` token in
  `Storage_engine_name` and exposes it through
  `option_storage_engine_name()`.
- `mariadb/sql/sql_table.cc:13430-13449` resolves explicit `ENGINE=` before
  the shallow `HA_CREATE_INFO` copy used for `CREATE TABLE` execution.
- `mariadb/sql/sql_table.cc:13611-13617` resolves omitted engines from the
  current session default storage engine.
- `mariadb/sql/sql_table.cc:13333-13404` applies
  `@@enforce_storage_engine` through `check_engine()`, updates
  `create_info->db_type`, and records the effective engine name in
  `create_info->new_storage_engine_name`.
- `mariadb/sql/handler.cc:6488-6512` suppresses durable `.frm` writes when the
  effective storage engine implements table discovery.
- `mariadb/storage/mylite/ha_mylite.cc` receives the effective MyLite handler
  call and can read the original `ENGINE=` token from `current_thd->lex` before
  storing catalog metadata.

## Proposed Design

Use MariaDB's existing storage-engine enforcement mechanism inside each
file-backed MyLite embedded connection:

- Set `sql_mode=''` for file-backed MyLite sessions so disabled engines such
  as `InnoDB` can fall back through MariaDB's normal substitution path instead
  of failing before handler enforcement.
- Set `default_storage_engine=MYLITE`.
- Set `enforce_storage_engine=MYLITE`.
- Leave `:memory:` runtimes on the old MyISAM-oriented bootstrap so explicit
  `ENGINE=MYLITE` continues to fail without a primary file.

The handler remains the policy gate. On `create()` it determines the requested
engine:

- if `ENGINE=` was omitted, record `DEFAULT`;
- if `ENGINE=` was present, read the original parsed engine token from
  `Storage_engine_name`;
- allow `DEFAULT`, `MYLITE`, `InnoDB`, `MyISAM`, `Aria`, and follow-up
  `BLACKHOLE`;
- reject every other explicit engine before calling MyLite storage.

For accepted requests, store:

- `requested_engine_name`: `DEFAULT` or the original application engine name;
- `effective_engine_name`: `MYLITE`.

This keeps the routing mechanism small and uses MariaDB's normal DDL path, but
prevents broad `enforce_storage_engine` behavior from accidentally publishing
unsupported engines into the MyLite catalog.

## Affected MariaDB Subsystems

- Embedded runtime startup arguments in `packages/libmylite/`.
- MyLite handler `create()` policy in `mariadb/storage/mylite/`.
- MariaDB `CREATE TABLE` engine resolution through
  `Storage_engine_name`, `check_engine()`, and `@@enforce_storage_engine`.
- MyLite catalog table-definition records in `packages/mylite-storage/`.

## Compatibility Impact

Compatibility remains partial. Applications can create metadata-only tables
using omitted/default engine requests and common `InnoDB`, `MyISAM`, `Aria`, or
BLACKHOLE clauses, then rediscover those tables after reopen. BLACKHOLE accepts
and discards row writes after the dedicated follow-up slice. Other statements
that need rows, indexes, transactions, foreign keys, or catalog-changing DDL
still fail or remain unsupported as documented.

Unsupported engine requests should fail explicitly instead of silently becoming
MyLite tables.

## DDL Metadata Routing Impact

The FRM image stored for a routed table is MariaDB's canonical table-definition
image for the effective MyLite handler. The catalog separately keeps the
requested engine token so later compatibility reporting, migration, and
semantics checks can distinguish `ENGINE=InnoDB` from `ENGINE=MYLITE` and
from omitted-engine DDL.

## Single-File And Embedded Lifecycle

The sidecar lifecycle gate from the previous slice remains the invariant:
routed DDL must leave the primary `.mylite` file as the only durable database
asset after clean close, with an empty MyLite-owned runtime root.

## Public API Or File-Format Impact

No `libmylite` public API changes are required. The internal
`mylite-storage` API needs a table-metadata reader so tests and later routing
code can inspect requested/effective engine names without parsing raw catalog
bytes.

The file format already stores requested and effective engine strings in table
definition records; this slice exposes and exercises those existing fields.

## Implementation Status

Implemented. File-backed embedded connections configure the session default and
enforced storage engine to MyLite, clear `NO_ENGINE_SUBSTITUTION` through the
session SQL mode, and rely on the MyLite handler whitelist before catalog
publication. The storage API exposes requested/effective engine metadata for
tests and later routing code. Follow-up BLACKHOLE support extends the whitelist
with row-discard behavior while keeping MEMORY/HEAP planned.

## Storage-Engine Routing Impact

This slice is the first engine-routing policy. It intentionally covers only
metadata-safe requests:

- omitted engine / default engine,
- `ENGINE=MYLITE`,
- `ENGINE=InnoDB`,
- `ENGINE=MyISAM`,
- `ENGINE=Aria`,
- `ENGINE=BLACKHOLE` with row-discard behavior.

Other engines are not accepted until a slice documents their semantics.

## Wire Protocol Or Integration Impact

None directly. Future wire-protocol layers should inherit this behavior because
the policy lives below `mylite_exec()` in the embedded runtime and handler.

## Binary Size Impact

No MariaDB component is added. The runtime already links the static MyLite
storage engine in the storage-smoke profile. The new code is routing policy and
test coverage only.

## License Or Dependency Impact

No new dependency is introduced.

## Test And Verification Plan

1. Add a storage API test for reading requested/effective engine names from a
   catalog table-definition record.
2. Extend storage-engine smoke coverage so omitted engine, explicit
   `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and
   follow-up `ENGINE=BLACKHOLE` create routed MyLite tables and rediscover them
   after reopen.
3. Assert each routed table stores the requested engine and effective `MYLITE`
   engine in the catalog.
4. Assert an unsupported explicit engine such as `ENGINE=MEMORY` fails and does
   not publish a catalog record until its volatile row semantics are designed.
5. Reuse the sidecar scanner to prove routed DDL leaves no known MariaDB
   durable sidecars after close/reopen.
6. Run dev, embedded, and storage-smoke presets plus format, format-check,
   clang-tidy, and `git diff --check`.

## Acceptance Criteria

- File-backed `libmylite` runtimes route the covered engine requests to MyLite.
- `:memory:` behavior remains unchanged for explicit `ENGINE=MYLITE`.
- Requested and effective engine names are readable from the catalog and covered
  by tests.
- Unsupported explicit engines fail without adding catalog records.
- Routed DDL passes the no-sidecar and empty-runtime gates.
- Compatibility, storage architecture, and roadmap docs describe the partial
  routing policy without claiming row storage or full engine semantics.

## Risks And Unresolved Questions

- MariaDB's enforcement hook is deliberately broad, so the handler whitelist is
  mandatory. Missing that gate would silently accept unsupported engine names.
- The stored FRM image names the effective MyLite handler. The catalog-level
  requested engine string is therefore the compatibility source for later
  semantic checks and reporting.
- Some `InnoDB` features, especially foreign keys and engine-specific table
  options, may parse successfully before row/index support exists. Later slices
  must either implement them or reject them before storage publication.
