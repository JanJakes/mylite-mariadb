# Optimizer Trace Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's optimizer
trace implementation. Optimizer trace is a server diagnostic surface exposed
through the `optimizer_trace` system variables and
`information_schema.OPTIMIZER_TRACE`. It is useful for interactive server
debugging, but it is not part of MyLite's default file-owned embedded runtime.

Current baseline after `rpl-gtid-state-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,257,244 |
| `opt_trace.cc.o` object | 50,448 |
| stripped `mylite-open-close-smoke` | 5,750,512 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/sql/opt_trace.cc` implements optimizer trace startup,
  storage, privilege filtering, information-schema output, and several helper
  routines used by optimizer trace call sites.
- `vendor/mariadb/server/sql/opt_trace_context.h` embeds
  `Opt_trace_context opt_trace` in `THD`, so the type and lifecycle methods
  must continue to exist even when tracing is disabled.
- `vendor/mariadb/server/sql/sys_vars.cc` exposes `optimizer_trace` and
  `optimizer_trace_max_mem_size` through `Opt_trace_context::flag_names`.
- `vendor/mariadb/server/sql/sql_show.cc` registers
  `information_schema.OPTIMIZER_TRACE` through `Show::optimizer_trace_info` and
  `fill_optimizer_trace_info()`.
- `Json_writer::add_table_name()` and `Json_writer::add_str(Item*)` are defined
  in `opt_trace.cc` but are also used by non-trace JSON output, including
  EXPLAIN/ANALYZE and optimizer diagnostics. Those helpers must not become
  no-ops.

## Scope

Add a minsize option that removes the full optimizer trace implementation from
the embedded library. The option will:

- remove `../sql/opt_trace.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned optimizer trace stub;
- keep `optimizer_trace` system-variable parsing available but inert;
- keep `information_schema.OPTIMIZER_TRACE` available but empty; and
- preserve the JSON writer helpers that are shared with non-trace output.

## Non-Goals

- Do not remove optimizer-trace parser tokens or system variables.
- Do not remove JSON writer support used by EXPLAIN/ANALYZE.
- Do not remove `Opt_trace_context` from `THD` in this slice.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_OPTIMIZER_TRACE` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_optimizer_trace_stub.cc`. The
stub will:

- define `Show::optimizer_trace_info[]` so `sql_show.cc` still links;
- define `Opt_trace_context::flag_names` so `optimizer_trace` still parses;
- make `Opt_trace_context`, `Opt_trace_stmt`, and `Opt_trace_start` lifecycle
  methods inert;
- make trace-only functions such as `trace_condition()` and
  `print_final_join_order()` no-ops;
- return no rows from `fill_optimizer_trace_info()`; and
- copy the shared `Json_writer::add_table_name()` and `Json_writer::add_str()`
  helpers from MariaDB so non-trace JSON output still works.

## Affected Subsystems

- Embedded minsize SQL source list.
- Optimizer trace diagnostic behavior.
- Information schema `OPTIMIZER_TRACE` output.
- JSON writer helper definitions.
- Binary-size documentation.

## Single-File And Embedded-Lifecycle Impact

No file-format or lifecycle change. The slice removes per-session optimizer
trace allocation and diagnostic retention from the default embedded profile.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are close to the current 50,448-byte
`opt_trace.cc.o` member minus the replacement stub. Linked savings should be
larger than pure archive-only cuts because optimizer trace symbols are present
in the current open/close smoke.

Implemented measurements from `build/mariadb-minsize-no-optimizer-trace`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,257,244 | 30,229,492 | -27,752 |
| unstripped `mylite-open-close-smoke` | 7,992,176 | 7,983,152 | -9,024 |
| stripped `mylite-open-close-smoke` | 5,750,512 | 5,743,824 | -6,688 |

The embedded archive no longer contains `opt_trace.cc.o`. The replacement
`mylite_optimizer_trace_stub.cc.o` member is 22,104 bytes because it preserves
the shared JSON writer helpers used outside optimizer tracing.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-optimizer-trace \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-optimizer-trace \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-optimizer-trace \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `opt_trace.cc.o` in `libmariadbd.a`;
- presence of the replacement stub; and
- absence of substantive `Opt_trace_*` allocation/rendering symbols in the
  linked smoke.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- The embedded archive no longer contains `opt_trace.cc.o`.
- JSON helper behavior needed by existing smokes remains intact.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

All criteria passed for the current aggressive minsize profile.

## Risks And Unresolved Questions

- `optimizer_trace=enabled=on` will parse but produce no trace rows in the
  aggressive profile. That is acceptable for a size profile, but it must be
  documented as unsupported diagnostic behavior.
- JSON writer helpers in `opt_trace.cc` are shared with non-trace output; the
  stub must preserve them rather than deleting them.
