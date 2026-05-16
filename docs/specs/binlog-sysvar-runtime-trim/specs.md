# Binlog Sysvar Runtime Trim

## Problem

The default MyLite embedded profile disables binary logging, relay logs, GTID
binlog state, and replication execution, but MariaDB's `sys_vars.cc` still
registers many system variables for those server-topology features. That keeps
variable objects, option metadata, helper functions, and strings for surfaces
that the profile already rejects or compiles to MyLite stubs.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc` registers binlog variables such as
  `binlog_cache_size`, `binlog_format`, `sync_binlog`,
  `binlog_row_image`, `binlog_gtid_index`, and
  `binlog_large_commit_threshold`.
- The same file registers GTID and replication variables such as
  `gtid_binlog_state`, `gtid_domain_id`, `gtid_slave_pos`,
  `default_master_connection`, `slave_parallel_threads`, relay-log variables,
  `sql_slave_skip_counter`, and replication filters.
- The current embedded profile already sets `MYLITE_WITH_BINLOG_CORE=OFF` and
  `MYLITE_WITH_RPL_FILTER_RUNTIME=OFF`, replacing binlog transaction/event
  core, log-event server, GTID state, replication record/injector, and
  replication filter bodies with MyLite stubs.
- `packages/libmylite/src/database.cc` rejects representative replication and
  binlog SQL before MariaDB execution. The prior RPL filter trim added stable
  direct/prepared rejection for filter variable assignments.

## Official Documentation References

- MariaDB Binary Log documents the binary log as a server log used for
  replication and point-in-time recovery:
  <https://mariadb.com/docs/server/server-management/server-monitoring-logs/binary-log>
- MariaDB Replication Filters documents the `replicate_*` and `binlog_*`
  filter variables as replication configuration:
  <https://mariadb.com/kb/en/replication-filters/>
- MariaDB system-variable documentation treats these names as server
  configuration and introspection surfaces:
  <https://mariadb.com/kb/en/server-system-variables/>

## Scope

Add `MYLITE_WITH_BINLOG_SYSVARS`, defaulting to `ON` for ordinary MariaDB
builds and forced to `OFF` by the MyLite embedded baseline. When disabled in an
embedded build, omit registration for system variables that configure or expose
disabled binary-log, GTID-binlog, relay-log, or replication behavior.

Keep low-risk compatibility indicators that document disabled runtime state or
are common in dump/load workflows:

- `log_bin`, which reports `OFF`,
- `sql_log_bin`,
- `server_id`,
- `encrypt_binlog`, which reports disabled encrypted-binlog state.

## Non-Goals

This slice does not:

- remove the SQL parser, ordinary DDL/DML, prepared statements, or SQL value
  semantics,
- remove the residual no-binlog `MYSQL_BIN_LOG` shell,
- change non-embedded builds,
- change public `libmylite` ABI or file format,
- implement replication, relay logs, or binlogging, or
- claim full MariaDB `SHOW VARIABLES` compatibility for server-topology
  variables in the embedded profile.

## Proposed Design

- Add the build option in both MariaDB CMake entry points that can compile
  `sys_vars.cc`.
- Require `MYLITE_WITH_BINLOG_SYSVARS=OFF` to be paired with
  `MYLITE_WITH_BINLOG_CORE=OFF` for the embedded library.
- In `sys_vars.cc`, define `MYLITE_OMIT_BINLOG_SYSVARS` only for embedded
  builds where `MYLITE_WITH_BINLOG_SYSVARS=0`.
- Wrap binlog, relay-log, GTID-binlog, and replication-only sysvar declarations
  plus their private helper functions at declaration granularity, preserving
  upstream structure.
- Extend `libmylite` unsupported-surface policy so direct and prepared
  assignments to removed variables fail with stable MyLite diagnostics instead
  of depending on MariaDB's unknown-variable path.

## Affected Subsystems

- MariaDB embedded system-variable registration and `SHOW VARIABLES` output.
- Command-line option metadata retained through sysvar objects.
- Direct and prepared `libmylite` SQL unsupported-surface policy.
- Compatibility harness server-surface coverage.
- Embedded archive and linked smoke binary size.

## Compatibility Impact

Applications that inspect disabled binlog or replication variables such as
`binlog_format`, `gtid_binlog_state`, `relay_log`, or `replicate_do_db` will
see no `SHOW VARIABLES` rows in the MyLite embedded profile. Assignments to
removed binlog/replication variables are explicitly unsupported and fail before
MariaDB execution.

Common dump/load compatibility variables `sql_log_bin` and `server_id` remain
registered, and `log_bin` remains visible as `OFF`.

## Single-File and Embedded-Lifecycle Impact

The trim aligns variable registration with MyLite's file-owned embedded
lifecycle by not exposing knobs for server-owned binlog, relay-log, or
replication files. It does not change `.mylite` ownership, transient companion
policy, storage routing, or recovery behavior.

## Public API and File-Format Impact

No public C API, ABI, or `.mylite` file-format change.

## Storage-Engine Routing Impact

No storage routing change. Routed `ENGINE=InnoDB`, `ENGINE=MyISAM`,
`ENGINE=Aria`, `ENGINE=BLACKHOLE`, and `ENGINE=MEMORY` behavior remains the
current MyLite metadata and storage policy. Native replication/binlog behavior
for those engines remains out of scope.

## Binary-Size Impact

Expected impact is modest but measurable. The historical MyLite attempt at the
same source boundary reduced the old minsize archive by about 90 KiB and the
stripped open/close smoke by about 10 KiB. This slice must rerun measurements
against the current embedded profile before accepting the result.

## Test and Verification Plan

Run:

```sh
tools/mariadb-embedded-build configure
tools/mariadb-embedded-build build
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure
tools/mylite-size-report
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev
cmake --preset storage-smoke-dev
cmake --build --preset storage-smoke-dev
ctest --preset storage-smoke-dev
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
tools/mylite-compat-harness report server-surface
cmake --build --preset dev --target format
cmake --build --preset dev --target format-check
cmake --build --preset dev --target tidy
bash -n tools/mariadb-embedded-build tools/mylite-compat-harness
git diff --check
```

Also verify the default embedded archive no longer retains representative
strings or symbols for omitted variables, while `log_bin` remains observable as
`OFF`.

## Acceptance Criteria

- `MYLITE_WITH_BINLOG_SYSVARS` defaults to `ON` and is forced `OFF` only by the
  embedded baseline.
- The disabled profile omits representative binlog/replication variables from
  `SHOW VARIABLES`.
- `log_bin=OFF` remains visible.
- Direct and prepared assignments to removed binlog/replication variables
  return stable MyLite diagnostics.
- Docs, roadmap, and compatibility harness descriptions match the
  implementation.
- Size measurements are recorded from the current profile.

## Verification Results

Completed on 2026-05-16:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `tools/mylite-size-report`
- `cmake --preset embedded-dev`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev`
- `cmake --preset storage-smoke-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev`
- `cmake --preset dev`
- `cmake --build --preset dev`
- `ctest --preset dev`
- `tools/mylite-compat-harness report server-surface`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness`
- `git diff --check`

The default embedded archive is `26,450,480` bytes / `25.23 MiB` with `669`
members, down from `26,498,088` bytes by `47,608` bytes. The storage-smoke
archive is `26,645,872` bytes / `25.41 MiB` with `672` members, down from
`26,693,480` bytes by `47,608` bytes.

The default archive string probe for representative omitted and retained
variables returned only:

```text
log_bin
server_id
sql_log_bin
encrypt_binlog
```

## Risks and Unresolved Questions

- `SHOW VARIABLES` compatibility narrows for applications that inspect
  server-topology state. This is intentional for the embedded profile, but the
  retained variables should be revisited when real application suites are run.
- Further size wins around binary logging likely require replacing the residual
  `MYSQL_BIN_LOG` shell, which is a separate transaction/logging design slice.
