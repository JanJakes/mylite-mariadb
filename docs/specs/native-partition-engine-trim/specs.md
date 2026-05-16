# Native Partition Engine Trim

## Problem

The default embedded profile still registers MariaDB's native `partition`
storage-engine wrapper and merges `ha_partition.cc.o` into `libmariadbd.a`.
MyLite already rejects partition DDL before MariaDB execution because it does
not have partition metadata, per-partition catalog lifecycle, partition-aware
row/index routing, pruning, or recovery.

Keeping the native wrapper registered does not make partitioned tables
compatible. It only exposes a server-side handler surface that MyLite cannot
route to the single `.mylite` file yet.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt` registers `partition` through
  `MYSQL_ADD_PLUGIN(partition ha_partition.cc STORAGE_ENGINE DEFAULT
  STATIC_ONLY RECOMPILE_FOR_EMBEDDED)`.
- `mariadb/cmake/plugin.cmake` honors `PLUGIN_PARTITION=NO` for non-mandatory
  plugins. A probe configure with `PLUGIN_PARTITION=NO` removed `partition`
  from `EMBEDDED_PLUGIN_LIBS`.
- A probe build with `PLUGIN_PARTITION=NO` completed and produced an embedded
  archive measuring 20,699,784 bytes / 19.74 MiB with 480 members, versus the
  previous 21,020,816 bytes / 20.05 MiB with 481 members.
- The same probe showed `ha_partition.cc.o` absent from `libmariadbd.a`.
- The MariaDB SQL layer still compiles partition parser and validation code,
  including `mariadb/sql/sql_partition.cc`, even when the handler wrapper is
  disabled. This slice intentionally removes the native handler/plugin
  registration, not every parser-side partition reference.
- `docs/specs/partition-policy/specs.md` and storage-smoke tests already reject
  `CREATE TABLE ... PARTITION BY ...` and representative partition-management
  `ALTER TABLE` forms before catalog publication.
- A CSV trim probe with `PLUGIN_CSV=NO` did not disable CSV because
  `mariadb/storage/csv/CMakeLists.txt` marks CSV `MANDATORY`; CSV therefore
  needed a separate upstream-derived patch, now tracked by the native CSV
  engine trim.

## Design

- Force `PLUGIN_PARTITION=NO` in the MyLite embedded baseline.
- Keep the existing first-party partition DDL rejection policy unchanged.
- Extend MTR smoke profile validation to require `PLUGIN_PARTITION=NO` because
  the MTR smoke profile inherits the embedded baseline.
- Extend server-surface coverage so `SHOW ENGINES` does not advertise native
  `partition`.
- Extend size tooling and documentation so partition trim evidence is visible
  beside other embedded profile flags.

## Affected Subsystems

- MariaDB embedded CMake baseline.
- MTR smoke harness profile validation.
- Server-surface tests and compatibility documentation.
- Size-profile measurement documentation.

## MySQL/MariaDB Compatibility Impact

Native partitioned tables remain unsupported in the default embedded profile.
Applications that require MariaDB partitioned tables still need explicit MyLite
partition metadata and storage support before compatibility can be claimed.

This is not a compatibility regression for MyLite's current product contract:
partition DDL already fails with stable MyLite diagnostics before MariaDB
execution.

## DDL Metadata Routing Impact

No catalog format change. Partitioned table DDL continues to reject before
MyLite table metadata is published.

## Single-File And Embedded-Lifecycle Impact

The trim removes MariaDB's native partition wrapper from the embedded archive
and avoids advertising partition support that could imply external
per-partition table files or native engine metadata. Existing sidecar gates
remain the product invariant for routed application DDL.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Partition DDL remains blocked before storage-engine routing. Routed ordinary
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default-engine,
`ENGINE=BLACKHOLE`, and `ENGINE=MEMORY` / `ENGINE=HEAP` behavior remains
unchanged.

## Binary-Size Impact

The probe default embedded profile measured 20,699,784 bytes / 19.74 MiB with
480 archive members, a reduction of 321,032 bytes and one member from the
previous 21,020,816-byte / 481-member profile.

The implemented storage-smoke profile measures 20,895,184 bytes / 19.93 MiB
with 483 archive members, a reduction of 321,024 bytes and one member from the
previous 21,216,208-byte / 484-member profile while retaining the static MyLite
handler.

## License And Dependency Impact

No new dependency. The change removes one MariaDB-derived native partition
handler object from the disabled embedded profile.

## Test And Verification Plan

- Configure, build, and measure the default embedded archive.
- Configure, build, and measure the storage-smoke embedded archive with
  `PLUGIN_MYLITE_SE=STATIC`.
- Confirm default and storage-smoke archives omit `ha_partition.cc.o`.
- Confirm `SHOW ENGINES` does not advertise `partition`.
- Confirm existing partition DDL rejection tests still pass through direct,
  prepared, and storage-smoke compatibility groups.
- Confirm MTR smoke still builds and runs against the no-partition profile.
- Run the relevant CMake presets, CTest presets, compatibility harness groups,
  MTR smoke, format/static checks, and size report.

## Acceptance Criteria

- The embedded baseline sets `PLUGIN_PARTITION=NO`.
- Default and storage-smoke archives omit `ha_partition.cc.o`.
- `SHOW ENGINES` omits native `partition`.
- Partition DDL rejection behavior remains covered and unchanged.
- MTR smoke profile validation rejects stale partition-enabled caches.
- Size docs record the measured reduction.

## Risks And Open Questions

- Parser-side partition code still compiles because partition syntax is part of
  the SQL layer and MyLite uses explicit policy checks to reject unsupported
  DDL. Chasing every parser reference belongs to a later syntax-trim slice, if
  size evidence justifies it.
- Native CSV sidecars are handled by the separate native CSV engine trim; keep
  both trims covered in future embedded-profile rebuilds.
- Real partition support will need a separate storage design for table ids,
  per-partition row/index pages, pruning, copy ALTER, crash recovery, and lock
  behavior.
