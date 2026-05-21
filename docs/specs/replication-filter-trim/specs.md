# Replication Filter Trim

## Problem Statement

The default embedded profile already rejects replication and binary-log command
families and starts with binary logging disabled, but it still builds
MariaDB's replication and binlog filter runtime. It also exposes inherited
topology configuration variables such as `replicate_do_db`,
`replicate_wild_ignore_table`, and `binlog_do_db`.

Those filters only make sense for daemon-owned replication or binary logging.
They are not SQL execution, native storage, JSON, GEOMETRY/GIS, transaction, or
database-directory behavior.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/rpl_filter.cc` implements `Rpl_filter`, including database,
  table, wildcard-table, and rewrite filters used by replication and binlog
  code paths.
- `mariadb/sql/rpl_filter.h` declares the shared filter class and global
  `global_rpl_filter` / `binlog_filter` pointers used by retained MariaDB
  startup and generic logging code.
- `mariadb/sql/sys_vars.cc` registers filter system variables:
  `replicate_do_db`, `replicate_rewrite_db`, `replicate_do_table`,
  `replicate_ignore_db`, `replicate_ignore_table`,
  `replicate_wild_do_table`, `replicate_wild_ignore_table`,
  `binlog_do_db`, and `binlog_ignore_db`.
- `mariadb/sql/mysqld.cc` registers corresponding command-line options and
  routes option parsing through `Rpl_filter`.
- `mariadb/libmysqld/lib_sql.cc`, `mariadb/sql/mysqld.cc`,
  `mariadb/sql/keycaches.cc`, and `mariadb/sql/rpl_mi.cc` still allocate or
  copy `Rpl_filter` instances through shared startup paths, so the class
  symbols must remain available even when active filter parsing is omitted.

## Design

- Add `MYLITE_WITH_REPLICATION_FILTERS`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in the MyLite embedded baseline.
- When disabled, build `mylite_rpl_filter_disabled.cc` instead of
  `rpl_filter.cc`.
- Keep the `Rpl_filter` symbols available so retained MariaDB startup and
  generic no-binlog paths still link.
- Make disabled filters permissive for runtime checks (`db_ok()`,
  `db_ok_with_wild_table()`, and `tables_ok()` return true) and empty for
  introspection (`is_on()` false, getter strings empty). This preserves
  no-filter behavior without accepting unsupported topology configuration.
- Compile out replication and binlog filter system-variable registration in
  the embedded profile.
- Compile out the matching filter command-line option rows in the embedded
  profile.
- Leave ordinary SQL execution, prepared statements, native storage,
  transactions, JSON, GEOMETRY/GIS, and database-directory lifecycle untouched.

## Compatibility Impact

The default embedded profile no longer exposes replication or binary-log filter
configuration variables. Direct and prepared lookups of the omitted `@@`
variables fail with MariaDB's unknown-system-variable errno.

This is consistent with the existing policy: core `libmylite` is an embedded
database-directory runtime without daemon-owned replication, relay logs, binary
logging, or wire-protocol listener state. Custom profiles that deliberately
re-enable topology support keep the upstream filter runtime by leaving
`MYLITE_WITH_REPLICATION_FILTERS=ON`.

## Directory And Lifecycle Impact

No file-format change and no new database-directory companions. The disabled
runtime prevents inherited replication and binlog filter configuration from
creating or implying external topology state.

## Public API Impact

No `libmylite` C API change. The change affects only inherited server
configuration exposed through SQL system-variable lookup and startup options in
the default embedded profile.

## Native Storage Impact

No native storage-engine behavior changes. InnoDB, MyISAM, Aria, MEMORY,
BLACKHOLE, and other retained engine behavior stay governed by existing
coverage and support policy.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| PROXY protocol listener trimmed | 26,527,408 bytes / 25.30 MiB | 703 | baseline |
| Replication filter runtime trimmed | 26,515,136 bytes / 25.29 MiB | 703 | -12,272 bytes |

The pre-strip archive moved from 27,097,424 bytes to 27,085,072 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg 'rpl_filter'
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_REPLICATION_FILTERS=OFF` appears in the embedded CMake cache.
- `rpl_filter.cc.o` is absent from `libmariadbd.a`.
- `mylite_rpl_filter_disabled.cc.o` is present in `libmariadbd.a`.
- `SHOW VARIABLES` does not expose representative replication and binlog
  filter variables in the default embedded profile.
- Direct and prepared `@@replicate_do_db`, `@@replicate_wild_ignore_table`, and
  `@@binlog_do_db` lookups fail with MariaDB unknown-system-variable errno.
- Existing embedded coverage still proves supported SQL, prepared statements,
  native storage, JSON, GEOMETRY/GIS, transaction, and directory-lifecycle
  behavior.

## Risks

- Shared MariaDB code still allocates `Rpl_filter`, so this slice must keep an
  inert replacement rather than deleting the class.
- Future wire-protocol or replication-adjacent integration profiles that need
  filter semantics must opt back into the upstream runtime deliberately.
