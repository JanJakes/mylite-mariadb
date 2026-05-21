# Replication Execution Sysvar Trim

## Problem Statement

The default embedded profile still registered replication execution, slave
protocol, and semi-sync system variables even though replication and binlog
command families are outside the core `libmylite` contract. These variables
configure server topology behavior that the embedded profile already rejects or
starts without.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/sys_vars.cc` registers `slave_compressed_protocol`,
  `slave_exec_mode`, `slave_ddl_exec_mode`, `slave_run_triggers_for_rbr`,
  `slave_type_conversions`, replication checksum variables,
  `replicate_events_marked_for_skip`, and semi-sync master/slave variables.
- `mariadb/sql/sys_vars.cc` keeps compatibility variables such as `log_bin`,
  `server_id`, GTID position variables, and other shared SQL/runtime variables
  in separate declarations.
- `mariadb/sql/mysqld.cc`, `mariadb/sql/rpl_utility_server.cc`, and semi-sync
  sources still define shared globals or helpers referenced by retained MariaDB
  code. This slice omits only system-variable registration, not those shared
  helpers.
- `packages/libmylite/tests/embedded_server_surface_policy_test.c` already
  covers rejected replication/binlog command families, `@@log_bin=0`, and absent
  binlog/relay-log sidecars.

## Design

- Add `MYLITE_WITH_REPLICATION_EXEC_SYSVARS`, defaulting to `ON` for
  upstream-style builds and forced `OFF` in the MyLite embedded baseline.
- When disabled, compile out the replication execution, slave protocol,
  replication-event, checksum, and semi-sync variable registrations in the
  contiguous `sys_vars.cc` block.
- Keep common compatibility and status variables such as `@@log_bin=0` available
  where they are already part of the embedded policy contract.
- Keep retained shared replication helper objects until a separate source and
  link review proves narrower removals are safe.

## Compatibility Impact

The default embedded profile no longer exposes this narrow set of replication
execution variables. Replication and binlog SQL were already unsupported. This
does not remove SQL execution, prepared statements, transactions, JSON,
GEOMETRY/GIS, native storage engines, or database-directory lifecycle behavior.

## Directory And Lifecycle Impact

No file-format change and no new database-directory companions. The trim
removes configuration metadata for server topology behavior and does not affect
durable or transient MyLite-owned files.

## Public API Impact

No `libmylite` C API change. Direct and prepared SQL that references omitted
variables fails with MariaDB's unknown-system-variable errno.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| VIO TLS transport trimmed | 26,536,112 bytes / 25.31 MiB | 703 | baseline |
| Replication execution sysvars trimmed | 26,534,136 bytes / 25.30 MiB | 703 | -1,976 bytes |

The pre-strip archive moved from 27,106,496 bytes to 27,104,488 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
if nm build/mariadb-embedded/libmysqld/libmariadbd.a \
  | rg 'Sys_semisync|Slave_type_conversions|Sys_slave_compressed_protocol'; then
  exit 1
fi
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

- `MYLITE_WITH_REPLICATION_EXEC_SYSVARS=OFF` appears in the embedded CMake
  cache.
- Direct and prepared `@@slave_type_conversions` lookups fail with MariaDB
  unknown-system-variable errno.
- `SHOW VARIABLES` does not expose `slave_type_conversions` or
  `rpl_semi_sync_master_enabled` in the default embedded profile.
- `@@log_bin=0` remains covered by server-surface policy tests.
- JSON, GEOMETRY/GIS, native storage, transactions, and prepared statements
  remain covered by the existing embedded test suite.
- The embedded and non-embedded test suites pass.

## Risks

- Many inherited replication and GTID helper objects remain because generic
  MariaDB runtime code still references them. Further removal needs separate
  symbol, source, and compatibility review.
- Applications that inspect this narrow set of replication execution variables
  will see an unknown-variable error in the default embedded profile. That is
  consistent with replication being outside the core library contract.
