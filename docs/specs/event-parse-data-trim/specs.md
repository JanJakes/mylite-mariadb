# Event Parse Data Trim

## Problem Statement

The MyLite embedded profile already treats the MariaDB event scheduler as a
server-owned surface. Event DDL and scheduler control are rejected, but
`libmariadbd.a` still linked MariaDB's full `Event_parse_data` validation
object for parser compatibility. That object validates event schedules and
date ranges for a scheduler that is not present in the embedded core.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/CMakeLists.txt` compiled `event_parse_data.cc` into the
  embedded archive even though `HAVE_EVENT_SCHEDULER` is not defined for
  `libmysqld`.
- `mariadb/sql/sql_parse.cc` returns "embedded server" for event DDL when
  `HAVE_EVENT_SCHEDULER` is absent.
- `mariadb/sql/sql_yacc.yy` still references `Event_parse_data::new_instance`
  while parsing event statements, so the embedded archive needs a small
  parser-link implementation rather than a deleted symbol.
- Full event scheduling, loading, execution, and Information Schema support
  live in non-embedded event scheduler sources that are not linked into the
  default embedded archive.

## Proposed Design

Add `MYLITE_WITH_EVENT_PARSE_DATA`, defaulting to `ON` for normal MariaDB
builds and forced `OFF` in the MyLite embedded baseline. When disabled, the
embedded build links `mylite_event_parse_data_disabled.cc`, which only
constructs enough `Event_parse_data` state for parser-owned event syntax to
remain linkable.

MyLite policy rejects direct and prepared event DDL plus event metadata
commands before they reach the MariaDB parser.

## Compatibility Impact

No supported application-data behavior is removed. Events and scheduler
metadata are already out of scope for the core embedded profile. Ordinary
tables, native storage, JSON, GEOMETRY, and normal SQL execution are not
affected.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,480,216 bytes / 25.25 MiB with 701 members, down 4,744 bytes from the prior
26,484,960-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_EVENT_PARSE_DATA=OFF` appears in the embedded CMake
  cache.
- Confirm `event_parse_data.cc.o` is absent and
  `mylite_event_parse_data_disabled.cc.o` is present in `libmariadbd.a`.
- Verify direct and prepared event DDL and event metadata SQL fail through the
  MyLite server-surface policy.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The embedded archive omits MariaDB's full event parse-data validation object.
- The embedded archive keeps parser link compatibility through the MyLite stub.
- Event DDL and event metadata statements fail explicitly through MyLite
  policy coverage.
- Normal application SQL and native storage behavior remain covered by the
  existing test suite.
