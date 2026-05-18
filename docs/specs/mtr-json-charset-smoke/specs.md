# MTR JSON And Charset Smoke

## Problem

The opt-in MariaDB MTR smoke runner should keep expanding upstream SQL
compatibility evidence, but only with tests that pass under MyLite's embedded
MTR profile without normalizing broad suite output. The next small addition is
to cover JSON equality and additional charset/session behavior that does not
require native MyISAM, CSV, SSL, Sequence, or server-only runtime surfaces.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite-mtr-harness` owns the curated pass-gated embedded MTR list and
  runs exact suite cases through `mariadb-test-run.pl --embedded-server`.
- `mariadb/mysql-test/main/json_equals.test` exercises `JSON_EQUALS()` over
  object, array, `NULL`, empty-string, timestamp, Unicode, reordered-key, and
  recursive JSON inputs, plus a small cross-charset table comparison.
- `mariadb/mysql-test/main/ctype_filesystem.test` exercises filesystem
  character-set reporting and session charset restoration without table DDL.
- `mariadb/mysql-test/main/ctype_collate_implicit_utf32.test` exercises
  `character_set_collations` assignment through UTF-32 conversion and reset.

Rejected probes are intentionally not promoted:

- `main.ctype_utf16_def` fails on `ft_stopword_file` result drift in the
  trimmed embedded profile.
- `main.json_normalize`, `main.ansi`, and `main.ctype_utf8mb4_bin` fail on
  Aria-vs-MyISAM `SHOW CREATE TABLE` result drift.
- `main.func_digest` reports an MTR skip because SSL support is unavailable.
- `main.func_set` and `main.ctype_ucs2_uca` depend on native MyISAM paths.
- `main.ctype_utf8mb4_general_ci_casefold` and
  `main.ctype_utf8mb3_general_ci_casefold` require the Sequence engine.
- `main.ctype_dec8` reaches Oracle-mode routine syntax that is not part of the
  current embedded profile.
- `main.func_int` fails before completion in the current profile and remains
  outside the curated smoke list.

## Design

Add only the three passing upstream tests to `tools/mylite-mtr-harness`:

- `main.ctype_filesystem`
- `main.ctype_collate_implicit_utf32`
- `main.json_equals`

Keep the tests outside the default compatibility groups because the MTR runner
still builds `mariadbd` and upstream support tools. The tests remain opt-in but
strict: `run` must see each selected case report an MTR pass, while `probe`
continues to serve candidate discovery.

## Compatibility Impact

This slice expands upstream MariaDB compatibility evidence for:

- JSON equality scalar function behavior;
- filesystem charset session/reporting behavior;
- UTF-32 `character_set_collations` assignment and reset behavior.

It does not change MyLite SQL behavior, storage routing, file format, public
API, or supported server-surface policy.

## Single-File And Embedded Lifecycle Impact

No durable MyLite storage behavior changes. The promoted MTR tests run under
the existing embedded MTR smoke profile and do not change MyLite-owned file
lifecycle rules.

## Test And Verification Plan

- `tools/mylite-mtr-harness probe main.ctype_collate_implicit_utf32 main.ctype_utf16_def main.json_equals main.json_normalize main.func_digest main.func_set main.ansi`
- `tools/mylite-mtr-harness probe main.ctype_dec8 main.ctype_filesystem main.ctype_utf8mb4_bin main.ctype_utf8mb4_general_ci_casefold main.ctype_utf8mb3_general_ci_casefold main.ctype_ucs2_uca main.func_int`
- `tools/mylite-mtr-harness run main.ctype_filesystem main.ctype_collate_implicit_utf32 main.json_equals`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git diff --check`

## Acceptance Criteria

- The curated MTR list includes the three newly promoted tests.
- A strict selected `run` reports all three as MTR passes.
- Compatibility docs and roadmap text name the new JSON and charset coverage
  without claiming broad MTR-scale comparison.

## Risks And Follow-Up

Broad charset and JSON suites still need a separate normalization plan because
many upstream results encode native MyISAM, Sequence, SSL, or explicit server
runtime assumptions. The current slice deliberately records only pass-gated
coverage.
