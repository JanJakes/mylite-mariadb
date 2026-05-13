# Binlog Sysvar Size Profile

## Problem

The aggressive embedded minsize profile disables binary logging and
replication execution, but `sys_vars.cc` still registers many binary-log and
replication system variables. These retain variable objects, names, option
metadata, update hooks, and strings for server subsystems that cannot operate
in MyLite's embedded profile.

Current reference point after `auth-protocol-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 25,371,140 |
| stripped `mylite-open-close-smoke` | 4,538,104 |

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/sys_vars.cc` defines binary-log variables such as
  `binlog_cache_size`, `binlog_format`, `binlog_row_image`,
  `sync_binlog`, `log_bin_*`, GTID binlog variables, and replication/relay-log
  variables. They are still visible as strings in the linked smoke binary even
  though `MYLITE_DISABLE_BINLOG_CORE` and
  `MYLITE_DISABLE_BINLOG_REPLICATION` are enabled.
- `vendor/mariadb/server/sql/log.cc` still provides a small no-binlog
  `MYSQL_BIN_LOG` shell, `binlog_tp`, and `binlog_cache_dir`; this slice does
  not replace that object.
- `vendor/mariadb/server/sql/sql_builtin.cc.in` already omits the mandatory
  binlog daemon plugin under `MYLITE_DISABLE_BINLOG_CORE`, so plugin-level
  binlog sysvars are not registered in the embedded no-binlog profile.
- `tools/build-mariadb-minsize.sh` is the only place that should enable this
  new size profile flag.

## Official Documentation References

- MariaDB Binary Log documents the binary log as a server log used for
  replication and recovery:
  <https://mariadb.com/docs/server/server-management/server-monitoring-logs/binary-log>
- MariaDB Replication Filters documents the `replicate_*` and related filter
  options as replica filtering behavior:
  <https://mariadb.com/kb/en/replication-filters/>
- MariaDB system variable documentation treats these names as server
  configuration and introspection surfaces rather than SQL execution semantics:
  <https://mariadb.com/kb/en/server-system-variables/>

## Scope

Add `MYLITE_DISABLE_BINLOG_SYSVARS`, enabled only by the minsize build script.
When the flag is set for an embedded no-binlog build, omit registration of
system variables that configure or expose disabled binary-log, GTID-binlog,
relay-log, or replication behavior.

Keep variables that are commonly used by SQL clients or dumps and are harmless
with binlogging disabled, unless later measurement proves they are a meaningful
size root. In particular, this first pass keeps `sql_log_bin`, `last_gtid`, and
`server_id`, plus the harmless read-only `encrypt_binlog` disabled-state
variable.

## Non-Goals

This slice does not:

- remove SQL parser, optimizer, DDL, DML, `SHOW CREATE`, or prepared-statement
  behavior,
- remove the residual `MYSQL_BIN_LOG` shell or transaction coordinator path,
- change non-minsize builds,
- remove ordinary storage-engine behavior, or
- claim full MariaDB `SHOW VARIABLES` compatibility for the aggressive
  embedded minsize profile.

## Proposed Design

Add a new CMake option in `vendor/mariadb/server/libmysqld/CMakeLists.txt`:

- `MYLITE_DISABLE_BINLOG_SYSVARS`

Require it to be used with `MYLITE_DISABLE_BINLOG_CORE` and
`MYLITE_DISABLE_BINLOG_REPLICATION`.

In `sys_vars.cc`, define local preprocessor guards for the embedded minsize
profile and wrap binary-log/replication-only sysvar objects and their private
helper functions. Keep the guards at declaration granularity so upstream code
shape remains reviewable.

## Affected Subsystems

- System-variable registration and `SHOW VARIABLES` output.
- Command-line option metadata for binlog/replication variables exposed through
  sysvar registration.
- Linked string/data sections in the embedded smoke binary.

## Single-File and Embedded-Lifecycle Impact

This aligns the aggressive profile with the embedded lifecycle by not exposing
configuration knobs for binlog, relay-log, and replication files that the
profile cannot create or manage. It does not change `.mylite` file ownership,
transaction durability, or companion-file policy.

## Public API and File-Format Impact

No public `libmylite` API change and no file-format change.

Compatibility impact is limited to server-variable introspection and attempts
to set disabled binlog/replication variables in the aggressive minsize build.

## Binary-Size Impact

Measured against `auth-protocol-size-profile`, this slice reduced:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,371,140 | 25,281,364 | -89,776 |
| unstripped `mylite-open-close-smoke` | 6,468,904 | 6,440,584 | -28,320 |
| stripped `mylite-open-close-smoke` | 4,538,104 | 4,527,880 | -10,224 |
| `llvm-size` decimal total | 4,760,574 | 4,740,522 | -20,052 |

The largest remaining binlog-linked strings are status names and small
embedded lifecycle shells such as `binlog_cache_use`, `binlog_cache_disk_use`,
`binlog_cache_dir`, and `binlog_tp`. This slice deliberately does not replace
the remaining `MYSQL_BIN_LOG` shell.

The first attempt also removed `encrypt_binlog`, but the open/close smoke
caught that as a harmless read-only disabled variable used for introspection.
The final implementation keeps it.

## Test and Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh tools/run-embedded-bootstrap-smoke.sh tools/run-storage-engine-smoke.sh tools/run-compatibility-test-harness.sh
git diff --check
```

Measure:

- `libmysqld/libmariadbd.a`,
- unstripped and stripped `mylite-open-close-smoke`,
- `llvm-size` section deltas,
- absence of representative removed strings such as `binlog_row_image`,
  `gtid_binlog_state`, `gtid_domain_id`, `replicate_do_db`,
  `slave_parallel_threads`, and `sync_binlog`.

## Acceptance Criteria

- The new flag is off by default and enabled only in the minsize script.
- Non-minsize builds remain unchanged.
- Current MyLite smokes and compatibility harness pass.
- Removed variables are limited to disabled binary-log/replication surfaces.
- Size measurements are recorded in `docs/research/production-size-analysis.md`.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-binlog-sysvars MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

The open/close smoke now asserts representative disabled sysvar names are not
retained in the linked smoke binary.

## Risks and Unresolved Questions

- Some import tools issue harmless `SET sql_log_bin=0`; that variable is kept
  in this pass to avoid breaking common dump/load workflows unnecessarily.
- Applications that inspect binlog or replication variables will now see
  unknown-variable diagnostics in the aggressive profile. That is an explicit
  compatibility tradeoff, not a change to normal SQL execution.
- Further savings may require replacing the residual `MYSQL_BIN_LOG` shell,
  which is a separate transaction/logging design problem.
