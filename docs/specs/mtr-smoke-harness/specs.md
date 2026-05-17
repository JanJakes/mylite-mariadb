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
- [MTR numeric and date smoke](../mtr-numeric-date-smoke/specs.md) adds
  `main.bigint` and `main.adddate_454`.
- [MTR type and temporal rounding smoke](../mtr-type-rounding-smoke/specs.md)
  adds `main.type_ranges`, `main.type_num`, `main.type_uint`,
  `main.type_year`, `main.func_time_round`, `main.type_date_round`,
  `main.type_datetime_round`, `main.type_time_round`, and
  `main.type_timestamp_round`.
- [MTR type edge smoke](../mtr-type-edge-smoke/specs.md) adds
  `main.type_char`, `main.type_interval`, and `main.type_varbinary`.
- [MTR date format and ASCII charset smoke](../mtr-date-charset-smoke/specs.md)
  adds `main.date_formats`, `main.datetime_456`, and `main.ctype_ascii`.
- [MTR charset edge smoke](../mtr-charset-edge-smoke/specs.md) adds
  `main.ctype_cp850`, `main.ctype_cp866`, `main.ctype_hebrew`, and
  `main.ctype_utf32_def`.
- [MTR collation and diagnostics smoke](../mtr-collation-diagnostics-smoke/specs.md)
  adds `main.ctype_collate_database`, `main.ctype_collate_implicit`,
  `main.ctype_collate_implicit_def`, `main.ctype_collate_table`, and
  `main.ctype_errors`.
- [MTR collation column and context smoke](../mtr-collation-column-context-smoke/specs.md)
  adds `main.ctype_collate_column` and `main.ctype_collate_context`.
- [MTR temporal function smoke](../mtr-temporal-function-smoke/specs.md) adds
  `main.func_sapdb`, `main.func_time_64`, `main.func_timestamp`,
  `main.in_datetime_241`, `main.str_to_datetime_457`, and
  `main.type_time_6065`.
- [MTR parser and comparison smoke](../mtr-parser-comparison-smoke/specs.md)
  adds `main.brackets`, `main.comments`, and `main.compare`.
- [MTR IN predicate smoke](../mtr-in-predicate-smoke/specs.md) adds
  `main.func_in`.
- [MTR query and subselect smoke](../mtr-query-subselect-smoke/specs.md) adds
  `main.update_ignore_216`, `main.subselect_nulls`, and
  `main.subselect_extra`.
- [MTR order and union smoke](../mtr-order-union-smoke/specs.md) adds
  `main.order_by_zerolength-4285`, `main.order_fill_sortbuf`, and
  `main.subselect_union_rand`.
- [MTR aggregate DISTINCT smoke](../mtr-aggregate-distinct-smoke/specs.md)
  adds `main.count_distinct` and `main.sum_distinct`.
- It was also extended by
  [MTR operator smoke](../mtr-operator-smoke/specs.md) to include
  `main.func_equal` and `main.func_op`.
- [MTR string and format function smoke](../mtr-string-format-smoke/specs.md)
  adds `main.func_concat` and `main.func_format`.
- [MTR scalar function smoke](../mtr-scalar-function-smoke/specs.md) adds
  `main.func_bit`, `main.func_extract`, and `main.func_replace`.
- [MTR REGEXP smoke](../mtr-regexp-smoke/specs.md) adds
  `main.func_regexp` and `main.func_regexp_pcre`.
- [MTR DEFAULT and weight string smoke](../mtr-default-weight-string-smoke/specs.md)
  adds `main.func_default` and `main.func_weight_string`.
- [MTR KDF smoke](../mtr-kdf-smoke/specs.md) adds `main.func_kdf` and allows
  optional quoted MTR variant suffixes in pass-result assertions.
- [MTR disabled DES smoke](../mtr-disabled-des-smoke/specs.md) adds
  `main.func_encrypt_nossl`.
- [MTR require-pass harness](../mtr-require-pass-harness/specs.md) makes
  selected tests fail unless MTR reports an actual `[ pass ]` result.

## Design

Add `tools/mylite-mtr-harness` with two commands:

- `list` prints the curated smoke test list;
- `run [suite.test...]` builds the required MariaDB MTR support targets under
  `build/mariadb-mtr-smoke` with `cmake/mariadb-mtr-smoke.cmake` and runs each
  selected test with `mariadb-test-run.pl --embedded-server --skip-rpl`.

The runner anchors exact selected case names and requires the matching MTR
summary line to report `[ pass ]`. A selected test that is skipped by MTR
feature checks is treated as no coverage and fails the harness.

The default curated list remains intentionally baseline-oriented:

- `mylite.bootstrap_schema`.
- `main.cast`.
- `main.case`.
- `main.bigint`.
- `main.type_ranges`.
- `main.type_num`.
- `main.type_uint`.
- `main.type_char`.
- `main.type_interval`.
- `main.type_varbinary`.
- `main.type_year`.
- `main.adddate_454`.
- `main.date_formats`.
- `main.datetime_456`.
- `main.in_datetime_241`.
- `main.str_to_datetime_457`.
- `main.func_time_round`.
- `main.func_sapdb`.
- `main.func_time_64`.
- `main.func_timestamp`.
- `main.type_date_round`.
- `main.type_datetime_round`.
- `main.type_time_round`.
- `main.type_time_6065`.
- `main.type_timestamp_round`.
- `main.brackets`.
- `main.comments`.
- `main.compare`.
- `main.func_in`.
- `main.update_ignore_216`.
- `main.subselect_nulls`.
- `main.subselect_extra`.
- `main.order_by_zerolength-4285`.
- `main.order_fill_sortbuf`.
- `main.subselect_union_rand`.
- `main.ctype_ascii`.
- `main.ctype_cp850`.
- `main.ctype_cp866`.
- `main.ctype_hebrew`.
- `main.ctype_utf32_def`.
- `main.ctype_collate_column`.
- `main.ctype_collate_context`.
- `main.ctype_collate_database`.
- `main.ctype_collate_implicit`.
- `main.ctype_collate_implicit_def`.
- `main.ctype_collate_table`.
- `main.ctype_errors`.
- `main.count_distinct`.
- `main.sum_distinct`.
- `main.func_equal`.
- `main.func_op`.
- `main.func_bit`.
- `main.func_concat`.
- `main.func_default`.
- `main.func_extract`.
- `main.func_format`.
- `main.func_replace`.
- `main.func_regexp`.
- `main.func_regexp_pcre`.
- `main.func_weight_string`.
- `main.func_kdf`.
- `main.func_encrypt_nossl`.

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
  `main.bigint`, `main.type_ranges`, `main.type_num`, `main.type_uint`,
  `main.type_char`, `main.type_interval`, `main.type_varbinary`,
  `main.type_year`, `main.adddate_454`, `main.date_formats`,
  `main.datetime_456`, `main.in_datetime_241`, `main.str_to_datetime_457`,
  `main.func_time_round`, `main.func_sapdb`, `main.func_time_64`,
  `main.func_timestamp`, `main.type_date_round`, `main.type_datetime_round`,
  `main.type_time_round`, `main.type_time_6065`,
  `main.type_timestamp_round`, `main.brackets`, `main.comments`,
  `main.compare`, `main.func_in`, `main.update_ignore_216`,
  `main.subselect_nulls`, `main.subselect_extra`,
  `main.order_by_zerolength-4285`, `main.order_fill_sortbuf`,
  `main.subselect_union_rand`, `main.ctype_ascii`,
  `main.ctype_cp850`, `main.ctype_cp866`, `main.ctype_hebrew`,
  `main.ctype_utf32_def`,
  `main.ctype_collate_column`, `main.ctype_collate_context`,
  `main.ctype_collate_database`, `main.ctype_collate_implicit`,
  `main.ctype_collate_implicit_def`, `main.ctype_collate_table`,
  `main.ctype_errors`, `main.count_distinct`, `main.sum_distinct`,
  `main.func_equal`, `main.func_op`, `main.func_bit`,
  `main.func_concat`, `main.func_default`, `main.func_extract`,
  `main.func_format`, `main.func_replace`, `main.func_regexp`,
  `main.func_regexp_pcre`, `main.func_weight_string`, `main.func_kdf`, and
  `main.func_encrypt_nossl`.
- The runner builds the required MTR support targets from a fresh enough
  `build/mariadb-mtr-smoke` tree.
- `mylite.bootstrap_schema`, `main.cast`, `main.case`, `main.bigint`,
  `main.type_ranges`, `main.type_num`, `main.type_uint`, `main.type_char`,
  `main.type_interval`, `main.type_varbinary`, `main.type_year`,
  `main.adddate_454`, `main.date_formats`, `main.datetime_456`,
  `main.in_datetime_241`, `main.str_to_datetime_457`,
  `main.func_time_round`, `main.func_sapdb`, `main.func_time_64`,
  `main.func_timestamp`, `main.type_date_round`, `main.type_datetime_round`,
  `main.type_time_round`, `main.type_time_6065`,
  `main.type_timestamp_round`, `main.brackets`, `main.comments`,
  `main.compare`, `main.func_in`, `main.update_ignore_216`,
  `main.subselect_nulls`, `main.subselect_extra`,
  `main.order_by_zerolength-4285`, `main.order_fill_sortbuf`,
  `main.subselect_union_rand`, `main.ctype_ascii`,
  `main.ctype_cp850`, `main.ctype_cp866`, `main.ctype_hebrew`,
  `main.ctype_utf32_def`,
  `main.ctype_collate_column`, `main.ctype_collate_context`,
  `main.ctype_collate_database`, `main.ctype_collate_implicit`,
  `main.ctype_collate_implicit_def`, `main.ctype_collate_table`,
  `main.ctype_errors`, `main.count_distinct`, `main.sum_distinct`,
  `main.func_equal`, `main.func_op`, `main.func_bit`,
  `main.func_concat`, `main.func_default`, `main.func_extract`,
  `main.func_format`, `main.func_replace`, `main.func_regexp`,
  `main.func_regexp_pcre`, `main.func_weight_string`, `main.func_kdf`, and
  `main.func_encrypt_nossl` pass under
  `mariadb-test-run.pl --embedded-server` with the MTR smoke profile.
- Documentation states that this is opt-in smoke coverage, not full MTR-scale
  comparison.

## Risks And Open Questions

- Building MTR prerequisites materially increases local build size. Use
  `rm -rf build/mariadb-mtr-smoke` or `rm -rf build` to reclaim it.
- The first curated test does not exercise MyLite storage; a later slice should
  decide how to compare MTR cases against MyLite's embedded API and routed
  storage without introducing daemon-only behavior into the core library.
