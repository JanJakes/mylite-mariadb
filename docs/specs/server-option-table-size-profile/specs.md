# Server Option Table Size Profile

## Problem

After the option-help-text profile, the aggressive MyLite minsize build still
retains `my_long_options[]` rows for server command-line options whose owning
subsystems are already compiled out or stubbed: binlog filtering, replication,
and dynamic plugin loading. The option help prose is gone, but the row names and
metadata remain in `.data`/`.rodata` and are still visible in linked artifacts.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/sql/mysqld.cc` defines `my_long_options[]`, which is
  consumed by `handle_options()` during server option parsing.
- The current minsize profile already defines
  `MYLITE_DISABLE_BINLOG_CORE`, `MYLITE_DISABLE_BINLOG_REPLICATION`,
  `MYLITE_DISABLE_RPL_FILTER`, and
  `MYLITE_DISABLE_DYNAMIC_PLUGIN_LOADING`.
- The disabled binlog and replication profiles already compile their runtime
  surfaces to no-op or unsupported behavior. Dynamic plugin loading reports
  unavailable and does not load plugin libraries.
- Before this slice, `my_long_options` was a 7,504-byte linked data symbol in
  `mylite-open-close-smoke`.

## Official Documentation References

- [MariaDB Binary Log](https://mariadb.com/docs/server/server-management/server-monitoring-logs/binary-log)
  describes the binary log as a server log used for replication and recovery
  and documents activation through `--log-bin`.
- [MariaDB Replication Filters](https://mariadb.com/kb/en/replication-filters/)
  documents replication filter options such as `replicate_do_db` as replica
  filtering behavior.
- [MariaDB Plugin Overview](https://mariadb.com/docs/server/reference/plugins/plugin-overview)
  documents `--plugin-load-add` as a `mariadbd` startup/configuration option
  for loading server plugins.

## Scope

This slice may:

- guard option-table rows for binlog, replication, replication filters, and
  dynamic plugin loading with existing MyLite minsize macros,
- leave unrelated option rows and defaults intact,
- keep non-minsize builds unchanged, and
- record measured size deltas.

## Non-Goals

This slice does not:

- add a new public MyLite option policy,
- remove startup options that still affect embedded bootstrap,
- remove system-variable declarations,
- change SQL syntax or diagnostics directly,
- make a broad pass over all `my_long_options[]` rows, or
- change non-minsize builds.

## Proposed Design

Use local `#if` guards around individual `my_long_options[]` initializers whose
owning subsystem is already disabled by an existing minsize macro:

- `MYLITE_DISABLE_BINLOG_CORE` for binlog file/logging options,
- `MYLITE_DISABLE_BINLOG_REPLICATION` for replication and relay-log options,
- `MYLITE_DISABLE_RPL_FILTER` for replication filtering options, and
- `MYLITE_DISABLE_DYNAMIC_PLUGIN_LOADING` for `plugin-load` options.

Do not introduce a separate flag. The row should disappear only when the
corresponding implementation surface is already unavailable.

## Affected Subsystems

- Server option table initialization in `mysqld.cc`.
- Minsize linked data and string tables.
- Production size analysis documentation.

## Single-File and Embedded Lifecycle Impact

No file-format or storage lifecycle change. The removed options configure
daemon-side binlog, replication, and dynamic plugin behavior that MyLite's
aggressive embedded profile already disables.

## Public API and File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

Compatibility impact is limited to command-line option parsing in the
aggressive embedded profile. Passing the removed server options to the embedded
runtime is no longer accepted, which matches their unavailable subsystems.

## Binary-Size Impact

Measured on top of `no-myisam-temp-spill-size-profile`:

- `libmariadbd.a`: 25,994,786 -> 25,991,050 bytes (-3,736).
- unstripped `mylite-open-close-smoke`: 6,696,024 -> 6,693,368 bytes
  (-2,656).
- stripped `mylite-open-close-smoke`: 4,708,544 -> 4,706,032 bytes (-2,512).
- `my_long_options`: 7,504 -> 5,376 bytes (-2,128).

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived build-profile change. It adds no new
dependency and changes no trademark-facing packaging.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-option-table-trim MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-option-table-trim MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-option-table-trim MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-option-table-trim MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh tools/run-compatibility-test-harness.sh tools/run-storage-engine-smoke.sh`
- `git diff --check`

## Acceptance Criteria

- The minsize build succeeds.
- MyLite open/close, storage-engine, and compatibility smokes pass.
- Removed option names such as `log-bin`, `replicate-do-db`, and
  `plugin-load` are absent from the linked open-close smoke binary.
- Measured size changes are recorded in
  `docs/research/production-size-analysis.md`.

## Risks and Unresolved Questions

- Future embedded bootstrap arguments must not rely on the removed inherited
  server option names.
- Further option-table pruning needs a row-by-row startup/default audit; the
  remaining table still carries real defaults and option aliases.
