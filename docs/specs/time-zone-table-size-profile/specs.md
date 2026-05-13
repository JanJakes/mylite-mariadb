# Time Zone Table Size Profile

## Problem Statement

MariaDB's embedded runtime still compiles `sql/tztime.cc`, including dynamic
loading of named time zones from `mysql.time_zone*` system tables. MyLite's
embedded profile does not own those server system tables and should avoid
runtime paths that open them. This slice tests replacing the table-backed time
zone loader with a smaller embedded implementation that keeps `SYSTEM` and
numeric offset time zones.

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/libmysqld/CMakeLists.txt` includes
  `../sql/tztime.cc` in the embedded SQL source list.
- `vendor/mariadb/server/sql/tztime.cc` defines the `Time_zone` subclasses,
  `my_tz_init()`, `my_tz_find()`, and `my_tz_free()`.
- The upstream `my_tz_init()` path opens `mysql.time_zone_leap_second`,
  `mysql.time_zone_name`, `mysql.time_zone`,
  `mysql.time_zone_transition_type`, and `mysql.time_zone_transition`.
- The upstream `my_tz_find()` path can open the `mysql.time_zone*` tables
  lazily to resolve named zones.
- The current `-Oz` minsize archive member for `tztime.cc.o` is 48,896 bytes.
  The linked smoke still contains `my_tz_find()`, `my_tz_init()`,
  `Time_zone_db`, `Time_zone_system`, `Time_zone_utc`, and
  `Time_zone_offset` symbols.

## Proposed Design

Add `MYLITE_DISABLE_TIME_ZONE_TABLES` as an off-by-default MariaDB CMake
option. When enabled for `libmysqld`, remove `../sql/tztime.cc` from the
embedded source list and link `mylite_tztime_stub.cc`.

The replacement keeps:

- `SYSTEM`,
- numeric offsets such as `+00:00` and `+01:00`,
- internal UTC conversion through `my_tz_UTC`,
- monotonicity checks used by timestamp-sensitive planning.

It omits:

- startup reads of `mysql.time_zone*` tables,
- lazy reads of `mysql.time_zone*` tables,
- named time zones such as `Europe/Prague` unless they are `SYSTEM`.

## Affected Subsystems

- Embedded startup time-zone initialization.
- `SET time_zone`.
- `CONVERT_TZ()` when called with named time zones.
- Timestamp and date/time conversion internals.

## Single-File And Embedded-Lifecycle Impact

This improves embedded lifecycle hygiene: the minsize profile no longer tries
to open inherited server time-zone system tables at startup or during
`my_tz_find()`.

No `.mylite` file-format or catalog change is intended.

## Public API Or File-Format Impact

No public `libmylite` API or file-format change.

SQL behavior changes in the aggressive minsize profile:

- `SET time_zone='+00:00'` remains supported.
- `SET time_zone='SYSTEM'` remains supported.
- `SET time_zone='Europe/Prague'` fails with MariaDB's
  `ER_UNKNOWN_TIME_ZONE`.
- `CONVERT_TZ()` with numeric offsets remains supported.
- `CONVERT_TZ()` with omitted named zones returns `NULL`.

## Binary-Size Impact

Measured against `build/mariadb-minsize-oz`, replacing upstream `tztime.cc.o`
with the embedded stub produced these deltas:

| Artifact | `-Oz` baseline | No time-zone tables | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,169,370 | 29,147,460 | -21,910 |
| `mylite/libmylite.a` | 122,784 | 122,792 | +8 |
| `storage/mylite/libmylite_embedded.a` | 388,440 | 388,440 | 0 |
| `mylite-open-close-smoke` | 7,755,552 | 7,748,552 | -7,000 |
| stripped `mylite-open-close-smoke` | 5,570,216 | 5,564,416 | -5,800 |
| stripped `mylite-compatibility-smoke` | 5,462,704 | 5,455,224 | -7,480 |

The archive member changed from `tztime.cc.o` at 48,896 bytes to
`mylite_tztime_stub.cc.o` at 27,608 bytes. The linked open-close `size` total
changed from 5,796,713 bytes to 5,788,457 bytes.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tz-tables \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tz-tables \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tz-tables \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tz-tables \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

The open/close smoke should verify numeric-offset conversion, `SYSTEM`, named
zone rejection, and named-zone `CONVERT_TZ()` returning `NULL`.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close, and compatibility smokes pass.
- The archive no longer contains `tztime.cc.o`.
- Size deltas are recorded in this spec and in
  `docs/research/production-size-analysis.md`.
- Startup and `my_tz_find()` do not open `mysql.time_zone*` tables in the
  minsize profile.

## Risks And Unresolved Questions

- Named time zones are useful SQL compatibility. Omitting them is acceptable
  only for the aggressive minsize profile until MyLite has an explicit
  single-file catalog design for time-zone metadata.
- The replacement relies on platform `localtime_r()` / `gmtime_r()` for
  `SYSTEM` and internal UTC conversions, matching the retained upstream system
  and UTC code paths.
- `sec_to_TIME()` in the stub is simpler than upstream's overflow-avoiding
  implementation. Current Linux/aarch64 builds use 64-bit `time_t`, but this
  should be revisited for 32-bit targets.
