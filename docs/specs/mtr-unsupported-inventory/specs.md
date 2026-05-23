# MTR Unsupported Inventory

## Goal

Make probed-but-unaccepted MariaDB MTR candidates visible in the MyLite MTR
harness with reason categories. The harness should distinguish accepted
coverage, known unsupported or profile-mismatched probes, and still
unclassified upstream tests.

## Non-Goals

- Do not add expected-failure execution to the default MTR smoke run.
- Do not normalize upstream result files in this slice.
- Do not claim unsupported tests as compatibility coverage.
- Do not make server-only, native-engine, replication, Performance Schema, or
  log-table surfaces supported.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- Source refs:
  - `tools/mylite-mtr-harness`: curated MTR list, `probe`, and `coverage`.
  - `mariadb/mysql-test/mariadb-test-run.pl`: upstream MTR runner used by the
    harness.
  - `mariadb/mysql-test/suite/sys_vars/t/*.test`: probed system-variable
    candidates and embedded skips.

Recent probes show three recurring non-coverage classes: upstream MTR skips for
embedded server, deliberate MyLite profile omissions such as InnoDB,
Performance Schema, replication, Sequence, native MyISAM, and log tables, and
profile-specific result text such as native MyISAM `SHOW CREATE TABLE` output.

## Compatibility Impact

This slice changes compatibility reporting only. It makes unsupported MTR
candidates auditable, but it does not change SQL, C API, storage-engine, or
file-format behavior.

## Design

Add a structured unsupported inventory to `tools/mylite-mtr-harness`. Each row
records a `suite.test` name, a compact reason category, and a short human
reason. Add:

- `list-unsupported` for tab-separated inventory output,
- `known-unsupported-*` counters in `coverage`,
- an `unclassified-upstream-test-files` counter that subtracts accepted
  upstream coverage and known unsupported upstream probes.

The accepted default and storage MTR lists remain the only runnable compatibility
coverage.

## File Lifecycle

No `.mylite` file lifecycle impact. The new command is read-only and does not
configure, build, or run MTR.

## Embedded Lifecycle And API

No `libmylite` API or embedded runtime behavior changes. The inventory documents
embedded-profile skips and unsupported profile surfaces.

## Build, Size, And Dependencies

No production binary-size, dependency, license, or MariaDB fork-delta impact.
The change is shell tooling and documentation only.

## Test Plan

- `tools/mylite-mtr-harness list-unsupported`
- `tools/mylite-mtr-harness coverage`
- Verify accepted and unsupported inventories do not overlap.
- `tools/mylite-mtr-harness run`
- `git diff --check`

## Acceptance Criteria

- The harness prints known unsupported/probed candidates with reason categories.
- Coverage output reports accepted, known unsupported, and unclassified upstream
  counts.
- Accepted MTR execution is unchanged and still passes.
- Documentation explains that known unsupported tests are not coverage.

## Risks And Open Questions

- The inventory is intentionally partial. Future probes should add rows when a
  failure is understood, but broad unprobed upstream MTR remains unclassified.
- Some currently unsupported MTR candidates may become acceptable after future
  profile changes or explicit normalization. Those rows should then move from
  the unsupported inventory into the accepted list with passing verification.
