# Append Query String Size Profile

## Problem Statement

The aggressive no-binlog profile still keeps
`vendor/mariadb/server/sql/log_event_server.cc` in the embedded archive. The
linked smoke no longer needs MariaDB's event reader/writer classes from that
file, but ordinary SQL rendering still calls `append_query_string()` for
quoted string output. Because that one helper lives in a large binlog event
translation source file, the archive keeps `log_event_server.cc.o`.

Current baseline after `tc-log-mmap-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,685,528 |
| `log_event_server.cc.o` archive member | 276,800 |
| stripped `mylite-open-close-smoke` | 5,751,536 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/sql/log_event_server.cc` implements
  `append_query_string()` at line 497. The helper reserves output space,
  renders dangerous character sets as hex through `str_to_hex()`, otherwise
  quotes the string and escapes it either with backslashes or doubled quotes.
- `vendor/mariadb/server/sql/sql_string.h` declares
  `append_query_string()` for non-binlog SQL code.
- Current non-binlog callers are:
  `vendor/mariadb/server/sql/item.cc`,
  `vendor/mariadb/server/sql/sql_acl.cc`, and
  `vendor/mariadb/server/sql/sql_type.cc`.
- The previous no-binlog follow-up already added
  `vendor/mariadb/server/libmysqld/mylite_log_event_core_stub.cc` for
  `str_to_hex()`, because ordinary SQL string rendering still needs it.
- `log_event_server.cc.o` is 276,800 bytes in the current stripped embedded
  archive. The linked binary only retains `append_query_string()` from that
  area, so expected linked-runtime savings are small, but archive savings
  should be measurable.

## Scope

When `MYLITE_DISABLE_BINLOG_CORE` is enabled:

- move the `append_query_string()` implementation into the existing
  `mylite_log_event_core_stub.cc` minsize source;
- keep the implementation byte-for-byte equivalent where practical;
- remove `../sql/log_event_server.cc` from `SQL_EMBEDDED_SOURCES`;
- leave `sql_string.h` and all callers unchanged; and
- keep non-minsize builds unchanged.

## Non-Goals

- Do not remove `append_query_string()` or change SQL string rendering
  semantics.
- Do not remove broader replication conversion helpers from
  `rpl_utility_server.cc`.
- Do not change parser grammar, public API, or `.mylite` file format.
- Do not remove `MYSQL_BIN_LOG` shell methods in this slice.

## Proposed Design

Extend `mylite_log_event_core_stub.cc` to contain both remaining
non-binlog-safe helpers:

- `str_to_hex()`, already present; and
- `append_query_string()`, copied from `log_event_server.cc` with only include
  adjustments.

Then update `vendor/mariadb/server/libmysqld/CMakeLists.txt` so
`MYLITE_DISABLE_BINLOG_CORE` removes `../sql/log_event_server.cc` and appends
the stub source. The existing stub is already included under the same option,
so no new CMake flag is needed.

## Affected Subsystems

- Embedded minsize SQL source list.
- SQL string rendering for generated SQL, account rendering, and type
  rendering paths that call `append_query_string()`.
- Binary-size documentation.

## Single-File And Embedded-Lifecycle Impact

No direct file-format or lifecycle change. The slice removes retained binlog
event-server code from the embedded archive and keeps only the SQL string
rendering helper required by ordinary execution paths.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are close to the current 276,800-byte
`log_event_server.cc.o` member, minus the small replacement helper. Expected
linked-runtime savings are much smaller because section GC already discarded
most event-server code from the final smoke binary.

Implemented measurements from `build/mariadb-minsize-no-log-event-server`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,685,528 | 30,385,682 | -299,846 |
| unstripped `mylite-open-close-smoke` | 7,993,536 | 7,993,104 | -432 |
| stripped `mylite-open-close-smoke` | 5,751,536 | 5,751,112 | -424 |

The embedded archive no longer contains `log_event_server.cc.o`. The
replacement `mylite_log_event_core_stub.cc.o` member is 2,280 bytes.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-log-event-server \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-log-event-server \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-log-event-server \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `log_event_server.cc.o` in `libmariadbd.a`; and
- retained `append_query_string()` symbol in the linked smoke.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- The embedded archive no longer contains `log_event_server.cc.o`.
- Ordinary SQL string rendering smoke coverage still passes through the
  compatibility harness.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

All criteria passed for the current aggressive minsize profile.

## Risks And Unresolved Questions

- `append_query_string()` is compatibility-sensitive. Even small escaping
  changes could affect generated SQL, so this slice should copy the inherited
  implementation rather than rewrite it.
- Linked-runtime savings may be minimal despite a useful archive reduction.
