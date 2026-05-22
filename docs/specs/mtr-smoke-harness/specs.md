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
- CMake accepts multiple targets after one `--target` flag, so the support
  target preparation does not need one `cmake --build` process per binary.
- The out-of-source build wrapper produces
  `build/mariadb-mtr-smoke/mysql-test/mariadb-test-run.pl` when run with the
  MTR smoke profile.
- [MTR support target build](../mtr-support-target-build/specs.md) batches the
  support target build through `tools/mariadb-embedded-build build`.
- [MTR run suite batching](../mtr-run-suite-batching/specs.md) runs accepted
  smoke tests one MTR process per suite while preserving per-test pass
  assertions.
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
- [MTR profile disabled engine smoke](../mtr-profile-disabled-engine-smoke/specs.md)
  adds `mylite.profile_disabled_engines`, covering selected native engines
  that the MyLite MTR smoke profile intentionally leaves out.
- [MTR profile disabled metadata smoke](../mtr-profile-disabled-metadata-smoke/specs.md)
  adds `mylite.profile_disabled_metadata`, covering selected status,
  process-list, and routine metadata producers that the MyLite MTR smoke
  profile intentionally compiles out.
- [MTR profile disabled diagnostics smoke](../mtr-profile-disabled-diagnostics-smoke/specs.md)
  adds `mylite.profile_disabled_diagnostics`, covering selected static `SHOW`,
  profiling, and optimizer-trace diagnostics that the MyLite MTR smoke profile
  intentionally compiles out.
- [MTR profile disabled file I/O smoke](../mtr-profile-disabled-file-io-smoke/specs.md)
  adds `mylite.profile_disabled_file_io`, covering selected host-file SQL I/O
  surfaces that the MyLite MTR smoke profile intentionally compiles out.
- [MTR profile disabled surface smoke](../mtr-profile-disabled-surface-smoke/specs.md)
  adds `mylite.profile_disabled_surfaces`, covering selected SQL surfaces that
  the MyLite MTR smoke profile intentionally compiles out.
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
- [MTR ANSI and binary smoke](../mtr-ansi-binary-smoke/specs.md) adds
  `main.ansi` and `main.binary`.
- [MTR numeric and date smoke](../mtr-numeric-date-smoke/specs.md) adds
  `main.bigint` and `main.adddate_454`.
- [MTR type and temporal rounding smoke](../mtr-type-rounding-smoke/specs.md)
  adds `main.type_ranges`, `main.type_num`, `main.type_uint`,
  `main.type_year`, `main.func_time_round`, `main.type_date_round`,
  `main.type_datetime_round`, `main.type_time_round`, and
  `main.type_timestamp_round`.
- [MTR hex-hybrid smoke](../mtr-hex-hybrid-smoke/specs.md) adds
  `main.type_hex_hybrid`.
- [MTR type edge smoke](../mtr-type-edge-smoke/specs.md) adds
  `main.type_char`, `main.type_interval`, and `main.type_varbinary`.
- [MTR binary and NCHAR smoke](../mtr-binary-nchar-smoke/specs.md) adds
  `main.type_binary` and `main.type_nchar`.
- [MTR VARCHAR smoke](../mtr-varchar-smoke/specs.md) adds
  `main.type_varchar`.
- [MTR date format and ASCII charset smoke](../mtr-date-charset-smoke/specs.md)
  adds `main.date_formats`, `main.datetime_456`, and `main.ctype_ascii`.
- [MTR charset edge smoke](../mtr-charset-edge-smoke/specs.md) adds
  `main.ctype_cp850`, `main.ctype_cp866`, `main.ctype_hebrew`, and
  `main.ctype_utf32_def`.
- [MTR JSON and charset smoke](../mtr-json-charset-smoke/specs.md) adds
  `main.ctype_filesystem`, `main.ctype_collate_implicit_utf32`, and
  `main.json_equals`.
- [MTR JSON normalize smoke](../mtr-json-normalize-smoke/specs.md) adds
  `main.json_normalize`.
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
- [MTR timezone smoke](../mtr-timezone-smoke/specs.md) adds `main.timezone4`.
- [MTR temporal, autoincrement, and optimizer smoke](../mtr-temporal-autoincrement-optimizer-smoke/specs.md)
  adds `main.temporal_scale_4283`, `main.second_frac-9175`,
  `main.mdev316`, `main.insert_update_autoinc-7150`,
  `main.strict_autoinc_3heap`, and `main.optimizer_costs2`.
- [MTR parser and comparison smoke](../mtr-parser-comparison-smoke/specs.md)
  adds `main.brackets`, `main.comments`, and `main.compare`.
- [MTR parser and expression smoke](../mtr-parser-expression-smoke/specs.md)
  adds `main.keywords`, `main.parser_stack`, `main.precedence`,
  `main.statement-expr`, `main.cte_cycle`, `main.name_const_replacement`,
  `main.implicit_char_to_num_conversion`, `main.item_types`, `main.round`, and
  `main.sql_safe_updates`.
- [MTR IN predicate smoke](../mtr-in-predicate-smoke/specs.md) adds
  `main.func_in`.
- [MTR query and subselect smoke](../mtr-query-subselect-smoke/specs.md) adds
  `main.update_ignore_216`, `main.subselect_nulls`, and
  `main.subselect_extra`.
- [MTR DML RETURNING smoke](../mtr-dml-returning-smoke/specs.md) adds
  `main.replace`, `main.bulk_replace`, `main.create_replace_tmp`,
  `main.key_primary`, `main.insert_returning_datatypes`,
  `main.replace_returning`, `main.replace_returning_datatypes`, and
  `main.replace_returning_err`.
- [MTR DDL and name smoke](../mtr-ddl-name-smoke/specs.md) adds
  `main.comment_column2`, `main.check`, `main.create_drop_db`,
  `main.lowercase_utf8`, `main.key_diff`, `main.check_constraint_show`,
  `main.constraints`, `main.create_drop_index`, `main.create-uca`, and
  `main.create_w_max_indexes_64`.
- [MTR comment DDL smoke](../mtr-comment-ddl-smoke/specs.md) adds
  `main.comment_column`, `main.comment_table`, and `main.comment_index`.
- [MTR order and union smoke](../mtr-order-union-smoke/specs.md) adds
  `main.order_by_zerolength-4285`, `main.order_fill_sortbuf`, and
  `main.subselect_union_rand`.
- [MTR ORDER BY optimizer smoke](../mtr-order-by-optimizer-smoke/specs.md)
  adds `main.order_by_optimizer` and `main.order_by-mdev-10122`.
- [MTR prepared statement smoke](../mtr-prepared-statement-smoke/specs.md)
  adds `main.prepare`, `main.ps_10nestset`, `main.ps_11bugs`,
  `main.ps_max_subselect-5113`, and `main.information_schema_prepare`.
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
- [MTR probe harness](../mtr-probe-harness/specs.md) adds a `probe` command
  for repeatable candidate discovery without treating skipped tests as
  coverage, prints a final probe summary, removes newly generated `.reject`
  files after probe failures, and applies a probe-only testcase timeout.

## Design

Add `tools/mylite-mtr-harness` with three commands:

- `list` prints the curated smoke test list;
- `run [suite.test...]` builds the required MariaDB MTR support targets in one
  batched build under `build/mariadb-mtr-smoke` with
  `cmake/mariadb-mtr-smoke.cmake`, groups selected tests by suite, and runs one
  MTR process per suite with `mariadb-test-run.pl --embedded-server --skip-rpl`;
- `probe suite.test...` uses the same exact MTR execution and pass-line
  assertion as `run`, but continues after failures so candidate suites can be
  evaluated in batches, prints a pass/fail summary, and removes newly generated
  `.reject` files so failed discovery runs do not dirty the source tree. Probe
  also passes a configurable testcase timeout to MTR so deadlock-oriented
  candidates do not stall local discovery indefinitely.

The runner anchors exact selected case names and requires the matching MTR
summary line to report `[ pass ]`. A selected test that is skipped by MTR
feature checks is treated as no coverage and fails `run` or the overall
`probe` command.

The default curated list remains intentionally baseline-oriented:

- `mylite.bootstrap_schema`.
- `mylite.profile_disabled_engines`.
- `mylite.profile_disabled_metadata`.
- `mylite.profile_disabled_diagnostics`.
- `mylite.profile_disabled_file_io`.
- `mylite.profile_disabled_surfaces`.
- `main.cast`.
- `main.case`.
- `main.ansi`.
- `main.bigint`.
- `main.type_ranges`.
- `main.type_num`.
- `main.type_uint`.
- `main.type_hex_hybrid`.
- `main.type_char`.
- `main.type_binary`.
- `main.binary`.
- `main.type_nchar`.
- `main.type_varchar`.
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
- `main.temporal_scale_4283`.
- `main.second_frac-9175`.
- `main.timezone4`.
- `main.brackets`.
- `main.comments`.
- `main.compare`.
- `main.keywords`.
- `main.parser_stack`.
- `main.precedence`.
- `main.statement-expr`.
- `main.cte_cycle`.
- `main.name_const_replacement`.
- `main.implicit_char_to_num_conversion`.
- `main.item_types`.
- `main.round`.
- `main.mdev316`.
- `main.sql_safe_updates`.
- `main.func_in`.
- `main.update_ignore_216`.
- `main.insert_update_autoinc-7150`.
- `main.strict_autoinc_3heap`.
- `main.replace`.
- `main.bulk_replace`.
- `main.create_replace_tmp`.
- `main.key_primary`.
- `main.insert_returning_datatypes`.
- `main.replace_returning`.
- `main.replace_returning_datatypes`.
- `main.replace_returning_err`.
- `main.comment_column`.
- `main.comment_table`.
- `main.comment_index`.
- `main.check_constraint_show`.
- `main.constraints`.
- `main.create_drop_index`.
- `main.create-uca`.
- `main.create_w_max_indexes_64`.
- `main.comment_column2`.
- `main.check`.
- `main.create_drop_db`.
- `main.lowercase_utf8`.
- `main.key_diff`.
- `main.subselect_nulls`.
- `main.subselect_extra`.
- `main.order_by_zerolength-4285`.
- `main.order_by_optimizer`.
- `main.order_by-mdev-10122`.
- `main.optimizer_costs2`.
- `main.order_fill_sortbuf`.
- `main.subselect_union_rand`.
- `main.prepare`.
- `main.ps_10nestset`.
- `main.ps_11bugs`.
- `main.ps_max_subselect-5113`.
- `main.information_schema_prepare`.
- `main.ctype_ascii`.
- `main.ctype_cp850`.
- `main.ctype_cp866`.
- `main.ctype_hebrew`.
- `main.ctype_utf32_def`.
- `main.ctype_filesystem`.
- `main.ctype_collate_column`.
- `main.ctype_collate_context`.
- `main.ctype_collate_database`.
- `main.ctype_collate_implicit`.
- `main.ctype_collate_implicit_def`.
- `main.ctype_collate_implicit_utf32`.
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
- `main.json_equals`.
- `main.json_normalize`.
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

- The runner lists the default curated tests shown above, including all
  follow-up additions recorded in this spec.
- The runner builds the required MTR support targets from a fresh enough
  `build/mariadb-mtr-smoke` tree with one batched build invocation.
- Strict `run` batches accepted tests by suite and still requires every
  selected test to report an MTR `[ pass ]` line.
- The full default curated list passes under
  `mariadb-test-run.pl --embedded-server` with the MTR smoke profile.
- Documentation states that this is opt-in smoke coverage, not full MTR-scale
  comparison.
- `probe` continues through multiple candidates and exits nonzero when any
  candidate fails or is skipped.

## Risks And Open Questions

- Building MTR prerequisites materially increases local build size. Use
  `rm -rf build/mariadb-mtr-smoke` or `rm -rf build` to reclaim it.
- The first curated test does not exercise MyLite storage; a later slice should
  decide how to compare MTR cases against MyLite's embedded API and routed
  storage without introducing daemon-only behavior into the core library.
