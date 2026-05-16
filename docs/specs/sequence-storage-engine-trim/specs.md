# Sequence Storage Engine Trim

## Problem

MariaDB has two sequence-related implementations:

- SQL sequence objects and value functions, implemented through
  `mariadb/sql/sql_sequence.cc` and `mariadb/sql/ha_sequence.cc`.
- The virtual `SEQUENCE` storage engine, implemented under
  `mariadb/storage/sequence/`.

MyLite already rejects SQL sequence object/value surfaces and replaces the SQL
sequence runtime in the default embedded profile. Before this slice, the
virtual `SEQUENCE` storage engine still remained enabled by default, so the
embedded profile could auto-discover magic tables such as `seq_1_to_10` that
bypass the MyLite catalog and storage-engine routing.

## Source Findings

- Base: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/sequence/CMakeLists.txt` registers the plugin as
  `MYSQL_ADD_PLUGIN(sequence sequence.cc STORAGE_ENGINE DEFAULT)`, so a normal
  MariaDB configure includes it unless `PLUGIN_SEQUENCE=NO` is forced.
- `mariadb/storage/sequence/sequence.cc` implements a handler that auto-creates
  virtual sequence tables. `parse_table_name()` recognizes names such as
  `seq_1_to_10` and `seq_1_to_10_step_3`.
- `discover_table()` synthesizes a table definition with one unsigned integer
  primary key column instead of reading MyLite catalog metadata.
- `discover_table_existence()` and `drop_table()` make matching names behave as
  engine-discovered tables even without explicit MyLite DDL.
- `maria_declare_plugin(sequence)` registers the visible storage engine name
  `SEQUENCE`.
- Official MariaDB documentation describes the storage engine as a default
  engine that creates virtual ephemeral tables automatically and distinguishes
  it from sequence objects:
  <https://mariadb.com/docs/server/server-usage/storage-engines/sequence-storage-engine>.
- MariaDB table-discovery documentation describes `seq_1_to_10` as an automatic
  discovery example for the Sequence engine:
  <https://mariadb.com/docs/server/reference/product-development/plugin-development/storage-engines-storage-engine-development/table-discovery>.

## Design

Set `PLUGIN_SEQUENCE=NO` in `cmake/mariadb-embedded-baseline.cmake`.

This removes the virtual storage engine from the default embedded archive and
from `SHOW ENGINES`. It keeps ordinary table names available to the MyLite
catalog. For example, a user-created table named `seq_1_to_10` should be a
normal MyLite-routed table when the plugin is absent, not a magic generated
table.

Do not add SQL-token rejection for every `seq_*_to_*` table reference. That
would overreach beyond the plugin boundary and could reject legitimate user
tables. The unsupported surface is MariaDB's automatic virtual sequence table
engine, not every identifier that resembles its discovery pattern.

## Affected Subsystems

- MariaDB embedded build profile.
- MariaDB plugin registry and `SHOW ENGINES` output.
- Storage-engine compatibility documentation and harness descriptions.

No first-party storage file format, MyLite handler implementation, public C API,
wire protocol package, or runtime SQL parser code changes are required.

## Compatibility Impact

The MariaDB-specific virtual `SEQUENCE` storage engine is out of scope for the
current embedded single-file profile. Applications that depend on
`SELECT * FROM seq_1_to_10` as an implicit generated table need a future
catalog-backed or query-planner-backed series feature before MyLite can claim
compatibility.

Ordinary `AUTO_INCREMENT` remains supported. SQL sequence objects and sequence
value expressions/functions remain separate unsupported surfaces from the
previous sequence-runtime trim.

## DDL Metadata And Storage Routing Impact

The trim prevents MariaDB from auto-discovering virtual tables outside MyLite's
catalog. It does not change routed `CREATE TABLE` metadata for omitted/default,
`MYLITE`, `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, or `HEAP`.

Unsupported explicit `ENGINE=SEQUENCE` requests should fail through the normal
storage-engine availability path because the engine is not registered.

## Single-File And Embedded-Lifecycle Impact

No durable file-format change is required. Removing the plugin strengthens the
single-file boundary because generated virtual tables cannot appear outside the
MyLite catalog-discovery path.

## Binary-Size Impact

The rebuilt default embedded archive is `27,180,040` bytes / `25.92 MiB` with
672 members, a reduction of `45,976` bytes and one archive member from the
previous sequence-runtime baseline. The rebuilt storage-smoke archive is
`27,360,624` bytes / `26.09 MiB` with 675 members, a reduction of `45,968`
bytes and one archive member.

Both archives omit `sequence.cc.o` and retain
`mylite_sql_sequence_disabled.cc.o` for the separate SQL sequence runtime
stub.

## Test And Verification Plan

1. Extend the storage-engine smoke test so `SHOW ENGINES` still reports
   `MYLITE` but does not report `SEQUENCE`, and so a table name that matches
   MariaDB's sequence pattern remains an ordinary MyLite catalog-backed table.
2. Reconfigure and rebuild the default MariaDB embedded archive.
3. Reconfigure and rebuild the storage-smoke MariaDB archive with
   `PLUGIN_MYLITE_SE=STATIC`.
4. Inspect both archives to confirm `sequence.cc.o` is absent while the MyLite
   disabled SQL sequence stub remains.
5. Run the focused storage-engine compatibility group and the full affected
   first-party presets.
6. Run formatting, static analysis, shell syntax, diff, size, and compatibility
   report checks.

## Acceptance Criteria

- `PLUGIN_SEQUENCE=NO` is part of the committed embedded baseline.
- `SHOW ENGINES` does not advertise `SEQUENCE` in the storage-smoke profile.
- A user-created `seq_1_to_10` table stores MyLite catalog metadata and reads
  as an ordinary table.
- The default and storage-smoke archives omit `sequence.cc.o`.
- Documentation distinguishes the virtual `SEQUENCE` storage engine from SQL
  sequence objects/value functions.
- Compatibility and size-profile docs include measured evidence.
- All relevant tests and checks pass.

## Risks

- Some MariaDB applications use `seq_*_to_*` generated tables for ad hoc series
  queries. MyLite intentionally leaves that unsupported until there is a
  catalog-aware or planner-aware design that fits the single-file runtime.
- A table named like `seq_1_to_10` may now be an ordinary user table in MyLite
  instead of a MariaDB virtual table. This is an intentional product-shape
  choice and should be documented as part of the sequence-engine boundary.
