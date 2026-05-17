# MTR Smoke Harness

## Problem

The compatibility harness groups MyLite-owned CTest coverage, but it still does
not run any MariaDB MTR cases. MTR is MariaDB's native SQL compatibility test
runner, so MyLite needs a small, proven entry point before broader MTR-scale
comparison work can be planned honestly.

This slice adds an opt-in smoke runner for curated embedded MTR tests. It is
intentionally separate from the default MyLite compatibility harness because
MTR preparation builds `mariadbd` and several upstream client/support tools.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/mariadb-test-run.pl` is MariaDB's MTR entry point.
- `mariadb/mysql-test/CMakeLists.txt` defines the upstream embedded MTR command
  with `--embedded-server`, `--skip-rpl`, and `MTR_BUILD_THREAD`.
- The out-of-source build wrapper produces
  `build/mariadb-mtr-smoke/mysql-test/mariadb-test-run.pl` when run with the
  MTR smoke profile.
- A real embedded smoke run requires more than
  `libmariadbd.a`: MTR probes for `mariadbd`, `mariadb-test-embedded`,
  `mariadb-client-test-embedded`, `my_safe_process`, common client binaries,
  Aria utility tools, `perror`, `mariadb-tzinfo-to-sql`, and `replace`.
- The MyLite MTR smoke profile omits native CSV, InnoDB, MyISAM, MRG_MyISAM,
  and partition. Its bootstrap-schema expectation is therefore
  profile-specific, omits CSV-backed `mysql.general_log` and `mysql.slow_log`,
  and lives in
  `mariadb/mysql-test/suite/mylite/t/bootstrap_schema.test`.
- Verified command:
  `tools/mylite-mtr-harness run mylite.bootstrap_schema`.
- `mariadb/mysql-test/main/cast.test` exercises MariaDB scalar CAST/CONVERT
  semantics, temporal precision conversion, numeric overflow and truncation
  warnings, character-set conversion, and result metadata through a mix of
  result-only and temporary-table statements without requiring a daemon-only
  test prelude.
- Verified command:
  `tools/mylite-mtr-harness run main.cast`.
- `mariadb/mysql-test/main/cast.test` normalizes `SHOW CREATE TABLE` default
  engine output for Aria-based smoke runs while preserving the upstream MyISAM
  expected result.
- The curated smoke list was later extended by
  [MTR CASE expression smoke](../mtr-case-expression-smoke/specs.md) to include
  `main.case`.
- It was also extended by
  [MTR operator smoke](../mtr-operator-smoke/specs.md) to include
  `main.func_equal` and `main.func_op`.
- [MTR string and format function smoke](../mtr-string-format-smoke/specs.md)
  adds `main.func_concat` and `main.func_format`.

## Design

Add `tools/mylite-mtr-harness` with two commands:

- `list` prints the curated smoke test list;
- `run [suite.test...]` builds the required MariaDB MTR support targets under
  `build/mariadb-mtr-smoke` with `cmake/mariadb-mtr-smoke.cmake` and runs each
  selected test with `mariadb-test-run.pl --embedded-server --skip-rpl`.

The default curated list is intentionally tiny:

- `mylite.bootstrap_schema`.
- `main.cast`.
- `main.case`.
- `main.func_equal`.
- `main.func_op`.
- `main.func_concat`.
- `main.func_format`.

This establishes a working MTR path while avoiding a false claim that MyLite has
meaningful MTR-scale coverage.

## Supported Scope

- MariaDB embedded MTR smoke runner.
- Curated upstream baseline test execution.
- Reuse of the MariaDB 11.8.6 embedded source through a separate MTR smoke
  build directory and CMake profile.

## Non-Goals

- Running MTR against MyLite storage-engine routing.
- Adding MTR to the default `tools/mylite-compat-harness run` group set.
- Broad SQL result normalization, flaky-test quarantine, parallel MTR shards,
  or CI dashboard integration.
- External daemon comparison.

## Compatibility Impact

The project gains its first executable MTR entry point, but compatibility status
does not move beyond partial. The roadmap should describe this as initial
embedded MTR smoke coverage, with MTR-scale comparison still planned.

## Single-File And Embedded-Lifecycle Impact

No MyLite file format or runtime behavior changes. The smoke runner exercises
MariaDB's embedded baseline under `build/mariadb-mtr-smoke/mysql-test/var`, not
MyLite `.mylite` storage.

## Build, Size, And Dependencies

No new dependency is added. The runner is a Bash script. The build impact
is significant when first run because it builds `mariadbd` and upstream MTR
client/support tools in the MTR smoke build tree; these are test
artifacts, not default MyLite linked-library artifacts.

## Test And Verification Plan

- `tools/mylite-mtr-harness list`.
- `tools/mylite-mtr-harness run`.
- Existing first-party format, tidy, dev, embedded, and storage-smoke checks
  should continue passing because the runner does not change production code.

## Acceptance Criteria

- The runner lists `mylite.bootstrap_schema`, `main.cast`, `main.case`,
  `main.func_equal`, `main.func_op`, `main.func_concat`, and
  `main.func_format`.
- The runner builds the required MTR support targets from a fresh enough
  `build/mariadb-mtr-smoke` tree.
- `mylite.bootstrap_schema`, `main.cast`, `main.case`, `main.func_equal`, and
  `main.func_op`, `main.func_concat`, and `main.func_format` pass under
  `mariadb-test-run.pl --embedded-server` with the MTR smoke profile.
- Documentation states that this is opt-in smoke coverage, not full MTR-scale
  comparison.

## Risks And Open Questions

- Building MTR prerequisites materially increases local build size. Use
  `rm -rf build/mariadb-mtr-smoke` or `rm -rf build` to reclaim it.
- The first curated test does not exercise MyLite storage; a later slice should
  decide how to compare MTR cases against MyLite's embedded API and routed
  storage without introducing daemon-only behavior into the core library.
