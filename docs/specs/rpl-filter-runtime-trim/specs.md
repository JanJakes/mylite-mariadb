# RPL Filter Runtime Trim

## Problem

MyLite already treats replication and binlog behavior as server topology, not
core embedded file-owned behavior. The default embedded archive still carries
MariaDB's `rpl_filter.cc`, which implements replication database/table include,
exclude, wildcard, and rewrite rules.

Those rules are only meaningful for a server-owned replication topology. The
embedded profile still initializes `binlog_filter` and `global_rpl_filter`
because retained MariaDB startup, table, and system-variable paths reference the
objects, but MyLite should not keep the full filter parser and rule storage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/rpl_filter.h` declares `Rpl_filter`, including public
  replication filter checks, setters, and getters.
- `mariadb/sql/rpl_filter.cc` implements database/table rule parsing, wildcard
  matching, rewrite rules, string formatting for system variables, and cleanup.
- `mariadb/libmysqld/lib_sql.cc` creates `binlog_filter` and
  `global_rpl_filter` if embedded startup has not already initialized them.
- `mariadb/sql/mysqld.cc` also creates and deletes the global filter objects.
- `mariadb/sql/sys_vars.cc` exposes replication and binlog filter variables
  through `Sys_var_rpl_filter` and `Sys_var_binlog_filter`.
- `mariadb/sql/sql_class.cc`, `mariadb/sql/table.cc`, and retained binlog/table
  paths call the permissive filter checks even when binlog is disabled.
- Before this slice, the default embedded archive contained `rpl_filter.cc.o`.

## Design

- Add `MYLITE_WITH_RPL_FILTER_RUNTIME`, defaulting to `ON` for upstream-like
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When the option is `OFF`, remove `../sql/rpl_filter.cc` from the embedded SQL
  source list and compile `mylite_rpl_filter_disabled.cc` instead.
- Keep `rpl_filter.h` unchanged because retained MariaDB sources still include
  it and use the `Rpl_filter` type.
- The disabled source defines the public `Rpl_filter` methods used by retained
  code:
  - checks are permissive and report no active filter;
  - setters accept and discard values so startup/default plumbing remains
    harmless;
  - getters return empty rule lists or empty strings;
  - database rewrite returns the original database name.
- Reject public direct and prepared MyLite `SET` assignments for replication
  and binlog filter variables before MariaDB execution. That keeps the public
  boundary explicit instead of silently accepting rule changes that the disabled
  runtime discards.

## Compatibility Impact

MariaDB Server supports replication filter system variables. MyLite's core
embedded profile does not support replication. Direct and prepared attempts to
set `replicate_do_db`, `replicate_ignore_db`, `replicate_do_table`,
`replicate_ignore_table`, `replicate_wild_do_table`,
`replicate_wild_ignore_table`, `replicate_rewrite_db`, `binlog_do_db`, or
`binlog_ignore_db` fail with a stable unsupported-surface diagnostic.

`SHOW VARIABLES` may still expose inherited variable names while broader
replication system-variable cleanup remains a separate size-profile slice.

## File-Format And Storage Impact

No file-format change and no storage behavior change. Ordinary routed SQL
continues to run as though no replication filters are configured.

## Binary-Size Impact

Historical branch evidence measured about 35 KiB of archive reduction and a
sub-KiB stripped linked-runtime reduction. This slice must remeasure against
the current embedded profile after recent server-surface trims.

## Implementation Result

The default embedded baseline now sets `MYLITE_WITH_RPL_FILTER_RUNTIME=OFF`.
Both the embedded and storage-smoke MariaDB archive source lists compile
`mylite_rpl_filter_disabled.cc` instead of `rpl_filter.cc`.

The disabled implementation keeps retained MariaDB references linkable and
permissive: `tables_ok()`, `db_ok()`, and wildcard database checks accept all
ordinary SQL paths, `is_on()` reports no active filter, setters discard values,
string getters return empty rule text, list getters expose empty lists, and
database rewrite returns the original database.

The public `libmylite` direct and prepared SQL policy rejects representative
replication/binlog filter assignments with the stable unsupported replication
filter diagnostic before MariaDB execution.

## Measured Binary-Size Impact

Measured on 2026-05-16 against the current profile:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Default embedded archive | 26,513,480 bytes / 25.29 MiB | 26,498,088 bytes / 25.27 MiB | -15,392 bytes |
| Storage-smoke archive | 26,708,872 bytes / 25.47 MiB | 26,693,480 bytes / 25.46 MiB | -15,392 bytes |

The archive member counts remain unchanged because the MyLite stub object
replaces the upstream object. The refreshed first-party linked smoke binaries
also shed the retained filter symbols; representative global-symbol counts
dropped from 15,080 to 15,063.

## Test And Verification Plan

- Add direct SQL policy coverage for replication/binlog filter assignments.
- Add prepared statement policy coverage for representative filter assignments.
- Build and measure the default embedded profile.
- Confirm the default embedded archive omits `rpl_filter.cc.o` and includes the
  MyLite disabled filter object.
- Run embedded, storage-smoke, and dev presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Run format, tidy, shell syntax, archive membership, symbol, and diff checks.

## Acceptance Criteria

- `MYLITE_WITH_RPL_FILTER_RUNTIME=OFF` is recorded in the default embedded
  baseline.
- The default embedded archive omits `rpl_filter.cc.o`.
- Public direct and prepared SQL reject representative replication/binlog
  filter assignments before MariaDB execution.
- Docs record the unsupported boundary and measured current size impact.

## Verification Results

Passed:

- `tools/mariadb-embedded-build configure && tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC && BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- archive membership checks for default and storage-smoke archives: only
  `mylite_rpl_filter_disabled.cc.o` is present
- removed-symbol check for private `Rpl_filter` parser/rule-storage helpers
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- `tools/mylite-size-report`
- `cmake --preset embedded-dev && cmake --build --preset embedded-dev && ctest --preset embedded-dev --output-on-failure`
- `cmake --preset storage-smoke-dev && cmake --build --preset storage-smoke-dev && ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-mtr-harness tools/mylite-size-report`
- `git diff --check`

## Risks And Open Questions

- This does not remove the replication filter system-variable declarations in
  `sys_vars.cc`; that is a broader replication/binlog sysvar trim.
- Retained MariaDB code may add more `Rpl_filter` method references during an
  upstream rebase. Link and symbol checks should catch that.
