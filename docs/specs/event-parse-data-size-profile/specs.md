# Event Parse Data Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's full
`event_parse_data.cc` implementation. MyLite already rejects
`CREATE EVENT`, `ALTER EVENT`, and `DROP EVENT` in embedded command dispatch
because event metadata lives in MariaDB's `mysql.event` server table. The
remaining parser data implementation only supports syntax that will be rejected
before event metadata execution.

Current baseline after `proxy-protocol-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,064,524 |
| `event_parse_data.cc.o` object | 13,952 |
| stripped `mylite-open-close-smoke` | 5,726,488 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/schema-object-ddl-rejection/specs.md` documents embedded
  rejection for `CREATE EVENT`, `ALTER EVENT`, and `DROP EVENT`.
- `vendor/mariadb/server/sql/sql_yacc.yy` allocates `Event_parse_data` while
  parsing `CREATE EVENT` and `ALTER EVENT`, before command dispatch rejects the
  statement in embedded builds.
- The current linked open/close smoke only retains
  `Event_parse_data::new_instance()` and the constructor from
  `event_parse_data.cc`.
- Full date, interval, definer, and validation helpers are only needed when
  event metadata execution can proceed into `Events::create_event()` or
  `Events::update_event()`, which the embedded profile blocks.

## Scope

Add a minsize option that removes the full event parser data implementation
from the embedded library. The option will:

- remove `../sql/event_parse_data.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned `Event_parse_data` parser-allocation stub;
- initialize fields that parser actions mutate; and
- continue relying on embedded command dispatch to reject event DDL.

## Non-Goals

- Do not implement event metadata storage.
- Do not emulate `mysql.event`.
- Do not change parser support for event syntax.
- Do not change non-embedded MariaDB behavior.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_EVENT_PARSE_DATA` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_event_parse_data_stub.cc`.
The stub will define `Event_parse_data::new_instance()` and the constructor.
It will not define validation helpers such as `check_parse_data()` or
`check_dates()` because embedded command dispatch rejects event DDL before
those helpers can be used.

## Affected Subsystems

- Embedded minsize SQL source list.
- Event DDL parser allocation.
- Binary-size documentation.

## DDL Metadata Routing Impact

No supported MyLite table DDL changes. Event metadata remains an unsupported
server metadata surface.

## Single-File And Embedded-Lifecycle Impact

This removes unused event scheduling validation code from the embedded runtime.
It does not change `.mylite` file ownership.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are bounded by the 13,952-byte
`event_parse_data.cc.o` member minus the small replacement stub. Linked-runtime
savings should be small because only parser allocation symbols are live in the
current smokes.

Measured result on top of `proxy-protocol-size-profile`:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,052,668 | -11,856 |
| `mylite_event_parse_data_stub.cc.o` object | 2,696 | replaces 13,952-byte `event_parse_data.cc.o` |
| `mylite/mylite-open-close-smoke` | 7,960,096 | -304 |
| stripped `mylite-open-close-smoke` | 5,726,400 | -88 |

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `event_parse_data.cc.o` in `libmariadbd.a`;
- presence and size of the replacement stub; and
- event DDL rejection evidence from bootstrap smoke.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-event-parse-data \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

The archive contains `mylite_event_parse_data_stub.cc.o` and no longer
contains `event_parse_data.cc.o`. The linked smoke retains only
`Event_parse_data::new_instance()` and the constructor. Embedded bootstrap
still reports explicit unsupported results for `CREATE EVENT`, `ALTER EVENT`,
and `DROP EVENT`, and the compatibility harness reports `status=0` for all
groups.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- Event DDL remains explicitly rejected in embedded bootstrap smoke.
- The embedded archive no longer contains `event_parse_data.cc.o`.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- This relies on embedded command dispatch continuing to reject event DDL
  before event validation or metadata execution. Future event support must
  disable this size option.
