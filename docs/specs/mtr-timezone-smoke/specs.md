# MTR Timezone Smoke

## Problem

The curated embedded MTR smoke list covers selected temporal functions and
rounding behavior, but only one timezone-oriented upstream test. Adding another
pass-gated timezone case improves temporal compatibility evidence without
taking on broad temporal suite normalization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/main/timezone.test` covers system time zone handling,
  CET daylight-saving transitions, `FROM_UNIXTIME()`, `UNIX_TIMESTAMP()`,
  timestamp warnings for spring time-gap values, timezone-table primary-key
  shape handling, and `%z` / `%Z` `DATE_FORMAT()` timezone output.
- `mariadb/mysql-test/main/timezone4-master.opt` starts the test with
  `--timezone=GMT+10`.
- `mariadb/mysql-test/main/timezone4.test` exercises `FROM_UNIXTIME()` and
  `UNIX_TIMESTAMP()` boundary behavior under that alternate timezone.
- `tools/mylite-mtr-harness` runs exact upstream suite cases with
  `mariadb-test-run.pl --embedded-server` and requires each promoted case to
  report an MTR pass.

Rejected probes are intentionally not promoted:

- `main.sysdate_is_now` fails because `SLEEP()` is unavailable in the current
  embedded profile.
- `main.1st` fails on expected system-database shape drift in the trimmed
  profile.
- `main.cache_temporal_4265` and `main.func_time_32` are MTR skips in the
  current build.
- `main.func_time`, `main.type_time`, and `main.type_datetime` hit explicit
  native-MyISAM requirements.
- `main.type_date` and `main.type_timestamp` reach host-file `SELECT ...
  INTO OUTFILE` coverage that is deliberately unsupported in MyLite.

`main.opt_trace_default` passed as an optimizer-trace default metadata check,
but is not promoted in this slice because optimizer trace is an intentionally
omitted server diagnostics surface in the default embedded profile. Keep that
under server-surface policy work rather than temporal SQL coverage.

## Design

Add `main.timezone` beside `main.timezone4` in the default MTR smoke list near
the existing temporal MTR tests. Do not add result normalization or alternate
expected files.

## Compatibility Impact

This expands upstream MariaDB compatibility evidence for system time zone and
timestamp conversion behavior under default and non-default server timezones.
It does not change MyLite SQL behavior, storage routing, public API, file
format, or single-file lifecycle policy.

## Test And Verification Plan

- `tools/mylite-mtr-harness probe main.sysdate_is_now main.timezone4 main.1st main.opt_trace_default`
- `tools/mylite-mtr-harness probe main.timezone4 main.cache_temporal_4265 main.func_time main.func_time_32`
- `tools/mylite-mtr-harness probe main.type_date main.type_time main.type_datetime main.type_timestamp`
- `tools/mylite-mtr-harness probe main.temporal_literal main.type_temporal_mysql56 main.type_timestamp_hires main.timezone`
- `tools/mylite-mtr-harness run main.timezone`
- `tools/mylite-mtr-harness run main.timezone4`
- `tools/mylite-mtr-harness run main.timezone main.timezone4`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git diff --check`

## Acceptance Criteria

- `main.timezone` and `main.timezone4` appear in the curated MTR smoke list.
- A strict selected run reports both tests as MTR passes.
- The full default MTR smoke run remains green.
- Roadmap and compatibility docs name the new timezone coverage without
  claiming broad temporal-suite coverage.

## Risks And Follow-Up

Most remaining temporal MTR suites still need broader normalization or explicit
unsupported-surface handling because they include native MyISAM, host-file I/O,
debug-only, or architecture-specific assumptions.
