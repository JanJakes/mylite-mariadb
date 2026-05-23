# MTR Unsupported Suite Selectors

## Problem

The MTR coverage inventory can list exact accepted tests and exact
known-unsupported probes, but many imported upstream suites are wholly outside
the current embedded profile. The first unclassified suites include binlog,
replication, Galera/wsrep, Performance Schema, native InnoDB, and partition
tests. Adding every file in those suites as an exact unsupported row would make
the harness brittle and would hide that the decision is suite-level policy, not
a per-file probe result.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/binlog/t` covers binary-log runtime behavior.
- `mariadb/mysql-test/suite/rpl/t` covers replication runtime behavior.
- `mariadb/mysql-test/suite/galera*/t` and
  `mariadb/mysql-test/suite/wsrep/t` cover Galera/wsrep runtime behavior.
- `mariadb/mysql-test/suite/perfschema*/t` covers Performance Schema runtime
  and metadata behavior.
- `mariadb/mysql-test/suite/plugins/t` covers server plugin loading,
  authentication, audit, password-check, compression-provider, and plugin
  metadata behavior.
- `mariadb/mysql-test/suite/sysschema/t` covers server-owned sys schema views
  and routines over Performance Schema, processlist, status, and native InnoDB
  observability surfaces.
- `mariadb/mysql-test/suite/versioning/t` covers system-versioned table
  metadata, history-row rewrites, transaction-registry state, and binlog,
  replication, partition, view, native InnoDB, and native engine variants.
- `mariadb/mysql-test/suite/period/t` covers application-time `PERIOD`
  metadata, `FOR PORTION OF` DML rewrites, period information-schema tables,
  trigger behavior, Performance Schema, and native InnoDB variants.
- `mariadb/mysql-test/suite/innodb*/t` covers native InnoDB engine internals,
  tablespaces, fulltext/GIS/zipped variants, and native InnoDB diagnostics.
- `mariadb/mysql-test/suite/parts/t` covers the partition engine.
- The imported `compat` suite files live under
  `mariadb/mysql-test/suite/compat/oracle/t`; they are Oracle-compatibility
  MTR cases.
- `mariadb/mysql-test/suite/encryption/t` covers key-management plugin files,
  native InnoDB and Aria encryption, binlog encryption, encrypted temporary
  files, and native recovery/import behavior.
- `docs/ROADMAP.md` and `docs/COMPATIBILITY.md` already describe binlog,
  replication/Galera, Performance Schema, server plugin surfaces, sys schema
  observability, native InnoDB, partitioning, and Oracle SQL mode as disabled,
  trimmed, or explicitly unsupported surfaces in the current embedded profile.
  MariaDB native
  encryption plugins, engine-specific encrypted sidecar behavior, and temporal
  table metadata are not part of the current MyLite single-file storage
  contract.

## Design

Extend `tools/mylite-mtr-harness` with unsupported suite selectors:

- exact `unsupported_tests` entries continue to describe probed individual
  files;
- new `unsupported_test_selectors` entries use shell-style imported test-name
  patterns such as `binlog.*`;
- `list-unsupported` expands selectors against the imported MTR inventory and
  prints concrete `suite.test` rows with their reason category;
- `coverage` and `list-unclassified` consume the expanded unsupported names,
  so selector-covered imported tests are removed from the unclassified count;
- the existing overlap guard remains: selector expansion must not overlap
  accepted curated tests.

The first selector set is deliberately limited to suites whose whole runtime is
outside MyLite's current embedded profile. Mixed suites such as `main`,
`sys_vars`, `engines`, charset suites, and JSON suites stay unclassified until
they are accepted, probed, or classified more narrowly.

## Compatibility Impact

This is inventory bookkeeping only. It does not mark the selected upstream MTR
files as accepted compatibility coverage and does not claim support for the
underlying server surfaces. It makes deliberate non-coverage visible so future
MTR expansion does not repeatedly rediscover the same out-of-profile suites.

## Single-File And Embedded Lifecycle Impact

No runtime or file-format change. The classified suites are outside the current
single-file embedded profile because they depend on server logs, replication,
native engines, partition metadata, or Performance Schema state.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency and no binary-size change. The harness remains a Bash script.

## Test And Verification Plan

- Run `bash -n tools/mylite-mtr-harness`.
- Run `tools/mylite-mtr-harness list-unsupported` and confirm selector-covered
  suites expand to concrete rows.
- Run `tools/mylite-mtr-harness coverage` and record the updated unsupported
  and unclassified counts.
- Run `tools/mylite-mtr-harness list-unclassified` and confirm no selected
  suite remains in the output.
- Run `git diff --check`.

## Verification Evidence

- `bash -n tools/mylite-mtr-harness`: passed.
- `tools/mylite-mtr-harness coverage`: accepted upstream coverage stayed at
  413 of 5,901 imported upstream files, known unsupported upstream files became
  3,091, and unclassified upstream files dropped to 2,397.
- `tools/mylite-mtr-harness list-unsupported` expanded the selector-backed
  categories to concrete rows:
  - `disabled-galera-runtime`: 674 rows.
  - `replication-surface`: 649 rows.
  - `native-innodb-profile`: 560 rows.
  - `disabled-performance-schema`: 464 rows.
  - `disabled-binlog-runtime`: 155 rows.
  - `disabled-partition-engine`: 143 rows.
  - `disabled-oracle-mode`: 89 rows, including 87 selector-expanded `compat`
    rows plus two earlier exact probes.
  - `disabled-native-encryption-profile`: 73 rows.
  - `disabled-plugin-surface`: 41 rows.
  - `disabled-sys-schema-surface`: 93 rows.
  - `unsupported-temporal-table-surface`: 43 rows.
- `tools/mylite-mtr-harness list-unclassified` no longer prints tests from
  `binlog`, `rpl`, `galera`, `galera_sr`, `galera_3nodes`,
  `galera_3nodes_sr`, `wsrep`, `perfschema`, `perfschema_stress`, `innodb`,
  `innodb_fts`, `innodb_gis`, `innodb_zip`, `parts`, `compat`, `plugins`,
  `encryption`, `sysschema`, `versioning`, or `period`.
- `git diff --check`: passed.

## Acceptance Criteria

- Exact unsupported probes remain supported.
- Suite selectors expand only to imported upstream MTR tests.
- Accepted curated MTR tests do not overlap unsupported selectors.
- Coverage counts distinguish accepted tests from known unsupported
  non-coverage.
- Documentation uses "probes and selectors" wording instead of implying every
  unsupported row was individually run.

## Risks

- A future MyLite profile may want to accept a narrow test from a selector
  covered suite. The existing overlap guard will catch that if the test is
  added to the accepted list; the selector must then be narrowed or removed.
- Whole-suite classification is appropriate only for suites with uniform
  out-of-profile prerequisites. Mixed suites must continue to use exact probes
  or narrower selectors.
