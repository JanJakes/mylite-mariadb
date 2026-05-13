# EXPLAIN Runtime Size Profile

## Problem Statement

The aggressive embedded minsize profile still links MariaDB's full
`sql_explain.cc` implementation. That code builds and prints tabular, JSON, and
`ANALYZE` query-plan diagnostics for `EXPLAIN`, `ANALYZE`, `SHOW EXPLAIN`, slow
log plan output, and optimizer-owned plan structures. Query-plan diagnostics are
useful during development, but they are not required for the smallest embedded
runtime profile.

## Source Base

- Base source: MariaDB Server `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Local baseline before this slice:
  `f318d7610bc6ac985a4769d394509b3585477a97`.

## Source Findings

- [vendor/mariadb/server/libmysqld/CMakeLists.txt](../../../vendor/mariadb/server/libmysqld/CMakeLists.txt)
  includes `../sql/sql_explain.cc` in `SQL_EMBEDDED_SOURCES`.
- [vendor/mariadb/server/sql/sql_explain.h](../../../vendor/mariadb/server/sql/sql_explain.h)
  documents that query optimization produces EXPLAIN structures separately from
  execution data structures, and that ANALYZE data tracks plan execution.
- [vendor/mariadb/server/sql/sql_parse.cc](../../../vendor/mariadb/server/sql/sql_parse.cc)
  dispatches `SQLCOM_SHOW_EXPLAIN`, `SQLCOM_SHOW_ANALYZE`, `EXPLAIN SELECT`,
  and `ANALYZE SELECT` through `lex->describe`, `lex->analyze_stmt`, and
  `lex->explain->send_explain()`.
- Ordinary SELECT, INSERT...SELECT, UPDATE, DELETE, UNION, TVC, and optimizer
  paths still call EXPLAIN helper functions to allocate, populate, or clean up
  plan structures even when the statement is not an EXPLAIN statement.
- Removing `sql_explain.cc.o` from the current archive leaves 33 undefined
  symbols in the linked open/close smoke, mostly `Explain_query` lifecycle
  methods, `Explain_*` vtables, small helper arrays, and a handful of plan-data
  collection methods.

## Scope

Add a minsize option that removes the full EXPLAIN/ANALYZE runtime from the
embedded library. The option will:

- remove `../sql/sql_explain.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned `mylite_explain_stub.cc`;
- keep ordinary query execution linkable by preserving no-op EXPLAIN lifecycle
  and plan-collection entry points; and
- reject EXPLAIN/ANALYZE/SHOW EXPLAIN execution with a stable unsupported
  diagnostic in the aggressive minsize profile.

## Non-Goals

- Do not remove ordinary `SELECT`, `INSERT`, `UPDATE`, `DELETE`, derived table,
  CTE, UNION, or optimizer execution.
- Do not remove `DESCRIBE table` / `SHOW COLUMNS` metadata syntax in this
  slice.
- Do not edit generated parser syntax in this slice.
- Do not change full MariaDB server behavior outside the MyLite minsize profile.
- Do not add public API or file-format changes.

## Proposed Design

Add `MYLITE_DISABLE_EXPLAIN_RUNTIME` to the embedded minsize profile. When
enabled:

- `libmysqld/CMakeLists.txt` replaces `../sql/sql_explain.cc` with
  `mylite_explain_stub.cc`.
- `mysql_execute_command()` rejects `SQLCOM_SHOW_EXPLAIN`,
  `SQLCOM_SHOW_ANALYZE`, and any statement with `lex->describe` or
  `lex->analyze_stmt` before execution reaches optimizer plan output.
- The stub defines the minimal EXPLAIN symbols still referenced by retained
  ordinary SQL code:
  - `create_explain_query*`, `delete_explain_query()`, and
    `print_explain_for_slow_log()`;
  - `Explain_query` lifecycle and send/print methods;
  - no-op `Explain_*` virtual methods needed for vtables; and
  - small data collection helpers such as `String_list::append_str()`,
    `Explain_index_use::set()`, `Explain_table_access::push_extra()`, and
    `Explain_range_checked_fer::*()`.

The retained lifecycle helpers should allocate enough state for ordinary
optimizer code that stores pointers, but all output methods should report the
unsupported EXPLAIN runtime if reached.

## Affected Subsystems

- Embedded minsize SQL source list.
- `mysql_execute_command()` execution rejection for EXPLAIN/ANALYZE surfaces.
- Optimizer plan-data helper symbols.
- Open/close smoke unsupported SQL coverage.
- Binary-size documentation.

## DDL Metadata Routing Impact

No DDL metadata routing change. This slice does not create, alter, drop, or
rename table metadata.

## Single-File And Embedded-Lifecycle Impact

No file-format, catalog, recovery, lock, or sidecar change. The slice removes
diagnostic plan-output code and should not add persistent files or companion
files.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

The current hidden-visibility baseline measures:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,117,602 |
| `sql_explain.cc.o` object | 256,808 |
| stripped `mylite-open-close-smoke` | 5,532,056 |

An archive-removal probe without a replacement stub reduced the archive by
274,612 bytes but failed to link due to the retained EXPLAIN helper surface.

The final `build/mariadb-minsize-no-explain-runtime` attempt replaces
`sql_explain.cc.o` with `mylite_explain_stub.cc.o`:

| Artifact | Hidden visibility baseline | EXPLAIN runtime omitted | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,117,602 | 28,896,338 | -221,264 |
| `mylite-open-close-smoke` | 7,705,056 | 7,663,144 | -41,912 |
| stripped `mylite-open-close-smoke` | 5,532,056 | 5,496,920 | -35,136 |
| stripped `mylite-compatibility-smoke` | 5,422,808 | 5,386,424 | -36,384 |

The replacement stub object is 47,848 bytes. The removed upstream
`sql_explain.cc.o` object was 256,808 bytes.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-explain-runtime \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-explain-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-explain-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-explain-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Add open/close smoke assertions that:

- ordinary `SELECT` still succeeds;
- `EXPLAIN SELECT 1` reports the unsupported EXPLAIN runtime;
- `ANALYZE SELECT 1` reports the unsupported EXPLAIN runtime; and
- `SHOW EXPLAIN FOR 1` or equivalent `SHOW ANALYZE` syntax reports the same
  unsupported diagnostic or a stable MariaDB validation diagnostic before plan
  output is attempted.

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `sql_explain.cc.o` in `libmariadbd.a`;
- presence and size of `mylite_explain_stub.cc.o`; and
- absence of compatibility-harness regressions.

## Acceptance Criteria

- The minsize build completes. Verified with
  `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-explain-runtime
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
  Verified with the commands in the test plan.
- Ordinary SELECT execution still works. Covered by existing smoke and harness
  statements.
- EXPLAIN/ANALYZE execution surfaces report stable unsupported diagnostics.
  Covered by open/close smoke checks for `EXPLAIN SELECT 1`,
  `ANALYZE SELECT 1`, and `SHOW EXPLAIN FOR 1`.
- The embedded archive no longer contains `sql_explain.cc.o`. Verified by
  archive listing; it contains `mylite_explain_stub.cc.o` instead.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks

- Optimizer and executor code still expects EXPLAIN helper objects to exist
  opportunistically. The replacement must preserve no-op lifecycle semantics
  rather than returning dangling pointers.
- Removing EXPLAIN is a real SQL compatibility loss. It belongs only in the
  most aggressive size profile.
- Slow-query-plan logging cannot emit plan text in this profile; the stub must
  fail closed rather than attempting partial output.
