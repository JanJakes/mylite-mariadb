# MTR Hires Time Smoke

## Problem

The curated embedded MTR smoke suite already covers temporal rounding and
several time-function paths, but it does not include MariaDB's
`main.func_time_hires` test. That test exercises microsecond precision for
time, datetime, timestamp, cast, and conversion functions. The test mostly runs
under MyLite's trimmed embedded profile, but `SHOW CREATE TABLE` output differs
because the harness default engine is Aria while upstream expected output was
recorded with MyISAM.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Source test: `mariadb/mysql-test/main/func_time_hires.test`.
- Expected result: `mariadb/mysql-test/main/func_time_hires.result`.
- The only probe diff was `ENGINE=Aria ... PAGE_CHECKSUM=1` versus
  `ENGINE=MyISAM` in two `SHOW CREATE TABLE` results.
- Other curated MTR tests already normalize this engine/PAGE_CHECKSUM
  difference with `--replace_result`.

## Proposed Design

- Add `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""`
  before the two `SHOW CREATE TABLE` statements in `func_time_hires.test`.
- Add `main.func_time_hires` to the curated MTR harness list near the existing
  time-function tests.
- Update compatibility and roadmap docs to record selected high-resolution time
  function coverage.

## Compatibility Impact

This adds test coverage only. It does not change SQL behavior, storage routing,
public API behavior, or file format. The normalization keeps upstream expected
results stable while accepting the harness default engine string.

## Single-File And Lifecycle Impact

No product file-lifecycle change. The MTR test uses ordinary temporary test
tables under the embedded smoke harness.

## Public API And File Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

No routing change. The test still runs with the harness default Aria engine and
normalizes `SHOW CREATE TABLE` output to the upstream MyISAM expectation.

## Binary-Size Impact

No binary-size or dependency change.

## Test And Verification Plan

- Probe `main.func_time_hires`.
- Run strict focused MTR for `main.func_time_hires`.
- Run the full curated MTR smoke suite.
- Run `tools/mylite-mtr-harness list | wc -l`.
- Run `bash -n tools/mylite-mtr-harness`.
- Run `git diff --check`.

## Acceptance Criteria

- `main.func_time_hires` reports an MTR pass under strict harness execution.
- The curated MTR smoke suite passes.
- No `.reject` files remain.
- Compatibility docs and roadmap mention the added high-resolution time
  function coverage.

## Risks And Unresolved Questions

- This is selected smoke coverage, not exhaustive temporal-function parity.
- Broader temporal edge cases remain owned by future MTR expansion and
  application-suite coverage.
