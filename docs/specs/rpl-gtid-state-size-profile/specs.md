# RPL GTID State Size Profile

## Problem Statement

The aggressive no-binlog embedded profile still compiles
`vendor/mariadb/server/sql/rpl_gtid.cc` into `libmariadbd.a`. Most of that
source implements replication and binlog GTID state manipulation that is not
needed when binary logging and replication are disabled for MyLite's embedded
runtime.

Current baseline after `append-query-string-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,385,682 |
| `rpl_gtid.cc.o` archive member | 123,368 |
| stripped `mylite-open-close-smoke` | 5,751,112 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/sql/rpl_gtid.h` defines `rpl_binlog_state_base` and
  `rpl_binlog_state` for GTID state storage, lookup, strict sequence checking,
  binlog state file IO, and string rendering.
- `vendor/mariadb/server/sql/rpl_gtid.cc` implements the full GTID state
  machinery, including hash storage, sequence allocation, IO cache read/write,
  and domain dropping.
- `vendor/mariadb/server/sql/log.cc` owns the static
  `rpl_global_gtid_binlog_state` and initializes/frees it through
  `setup_log_handling()` and log cleanup.
- Existing `MYLITE_DISABLE_BINLOG_CORE` guards in `log.cc` return inert values
  from binlog GTID APIs such as `write_gtid_event()`,
  `get_most_recent_gtid_list()`, `append_state_pos()`, `append_state()`,
  `is_empty_state()`, and strict-sequence checks.
- The linked open/close smoke currently retains only
  `rpl_binlog_state_base::init()`, `reset_nolock()`, `free()`, and
  destructors plus `rpl_binlog_state::init()`, `free()`, and destructors from
  `rpl_gtid.cc`.

## Scope

Add a minsize option that removes `rpl_gtid.cc` only when the no-binlog core
profile is active. The option will:

- require `MYLITE_DISABLE_BINLOG_CORE=ON`;
- remove `../sql/rpl_gtid.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned lifecycle stub for the retained
  `rpl_binlog_state_base` and `rpl_binlog_state` methods; and
- keep non-minsize MariaDB-derived embedded builds unchanged.

## Non-Goals

- Do not change GTID parsing helpers that remain reachable from system
  variables or diagnostics.
- Do not remove `rpl_utility_server.cc`; ALTER TABLE copy paths still use its
  field conversion helpers.
- Do not remove the `rpl_binlog_state` type declarations from MariaDB headers.
- Do not change parser grammar, public API, or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_RPL_GTID_STATE` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it from the
aggressive `tools/build-mariadb-minsize.sh` profile. Require
`MYLITE_DISABLE_BINLOG_CORE` because the stub is correct only after binlog
entry points have been compiled to no-ops.

Create `vendor/mariadb/server/libmysqld/mylite_rpl_gtid_state_stub.cc`. The
stub keeps constructor/destructor and init/free lifecycle compatibility for the
static `rpl_global_gtid_binlog_state`, but it does not initialize GTID hashes,
mutexes, dynamic arrays, or state-file helpers. The no-binlog guards in
`log.cc` must keep the richer methods unreachable; if a future source path
starts requiring them, the minsize build should fail instead of silently
pretending to support GTID binlog state.

## Affected Subsystems

- Embedded minsize SQL source list.
- Log subsystem startup/shutdown lifecycle for inert GTID state.
- Binary-size documentation.

## Single-File And Embedded-Lifecycle Impact

This slice removes remaining compiled GTID binlog state machinery from the
default embedded profile. It does not add or remove any `.mylite` files or
companion files.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are close to the current 123,368-byte
`rpl_gtid.cc.o` member minus the small replacement stub. Expected linked
runtime savings are small because section GC already discards most of the
object from the open/close smoke binary.

Implemented measurements from `build/mariadb-minsize-no-rpl-gtid-state`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,385,682 | 30,257,244 | -128,438 |
| unstripped `mylite-open-close-smoke` | 7,993,104 | 7,992,176 | -928 |
| stripped `mylite-open-close-smoke` | 5,751,112 | 5,750,512 | -600 |

The embedded archive no longer contains `rpl_gtid.cc.o`. The replacement
`mylite_rpl_gtid_state_stub.cc.o` member is 4,136 bytes. The linked smoke
retains only tiny `rpl_binlog_state` lifecycle symbols.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-rpl-gtid-state \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-rpl-gtid-state \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-rpl-gtid-state \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `rpl_gtid.cc.o` in `libmariadbd.a`; and
- retained `rpl_binlog_state` lifecycle symbols in the linked smoke.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- The embedded archive no longer contains `rpl_gtid.cc.o`.
- The linked smoke has no GTID state manipulation symbols beyond the lifecycle
  shell required by static construction and cleanup.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

All criteria passed for the current aggressive no-binlog minsize profile.

## Risks And Unresolved Questions

- This is intentionally valid only for no-binlog embedded builds. Enabling it
  with binary logging would break GTID state semantics.
- Linked-runtime savings may be minimal despite useful archive savings.
