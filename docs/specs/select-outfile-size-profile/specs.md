# Select Outfile Size Profile

## Problem Statement

MariaDB supports `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` for
server-side host-file export. MyLite's aggressive embedded profile already
omits `LOAD DATA` / `LOAD XML` host-file import execution. The matching
server-side export path can create arbitrary files outside the `.mylite`
lifecycle, so it has low embedded value and should not remain in the smallest
profile.

This slice rejects host-file SELECT export syntax and compiles out the retained
`select_export` / `select_dump` runtime while preserving `SELECT ... INTO`
variables.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant local source paths:

- `vendor/mariadb/server/sql/sql_yacc.yy:13495` parses
  `INTO OUTFILE ...` and constructs `sql_exchange` plus `select_export`.
- `vendor/mariadb/server/sql/sql_yacc.yy:13510` parses
  `INTO DUMPFILE ...` and constructs `sql_exchange` plus `select_dump`.
- `vendor/mariadb/server/sql/sql_yacc.yy:13433` constructs
  `select_dumpvar` for `SELECT ... INTO` variables; this must remain.
- `vendor/mariadb/server/sql/sql_class.cc:3391` implements
  `select_export` and `select_dump` host-file writing.
- `vendor/mariadb/server/sql/sql_class.cc:4067` implements `select_dumpvar`
  variable assignment and is unrelated to host-file export.
- `vendor/mariadb/server/sql/sql_class.h:6327` documents `sql_exchange` for
  non-database file IO, including both `INTO OUTFILE` and `LOAD DATA`.

Current linked symbol evidence from
`build/mariadb-minsize-no-sql-handler`:

| Symbol group | Visible linked bytes |
| --- | ---: |
| `select_export`, `select_dump`, and `sql_exchange` host-file export symbols | about 4.6 KiB |

## Design

Add a minsize option named `MYLITE_DISABLE_SELECT_OUTFILE`.

When enabled:

- parser actions for `INTO OUTFILE` and `INTO DUMPFILE` fail with
  `ER_NOT_SUPPORTED_YET`;
- `sql_class.cc` does not compile `select_to_file`, `select_export`, or
  `select_dump` host-file writer method bodies;
- `select_dumpvar` remains available for `SELECT ... INTO @var` and local
  stored-program variables;
- `sql_exchange` remains because inherited `LOAD DATA` parser state still uses
  it, even though MyLite's minsize profile already stubs `LOAD DATA`
  execution.

## Non-Goals

- Do not remove `SELECT ... INTO` variables.
- Do not change ordinary `SELECT`, result-set, or prepared-statement behavior.
- Do not remove `LOAD DATA` parser state in this slice.
- Do not remove generic mysys file APIs.

## Affected Subsystems

- Minsize CMake options and build script.
- MariaDB parser actions for host-file SELECT export.
- SQL result-interceptor implementation in `sql_class.cc`.
- MyLite open/close unsupported-profile smoke coverage.

## DDL Metadata Routing Impact

No table-definition routing change. This slice removes host-file result export
execution only.

## Single-File And Embedded-Lifecycle Impact

This is aligned with MyLite's file-owned runtime: host-file export can create
files outside the primary `.mylite` file and outside documented MyLite-owned
companions. Applications can still export result sets through the public API by
reading rows and writing their own files.

## Public API Or File-Format Impact

No public `libmylite` C API change and no `.mylite` file-format change.

SQL compatibility impact: the aggressive minsize profile no longer supports
`SELECT ... INTO OUTFILE` or `SELECT ... INTO DUMPFILE`.

## Binary-Size Impact

Measured savings on top of `sql-handler-size-profile` are small but real:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 26,434,272 | 26,414,746 | -19,526 |
| unstripped `mylite-open-close-smoke` | 6,853,264 | 6,847,392 | -5,872 |
| stripped `mylite-open-close-smoke` | 4,829,616 | 4,825,696 | -3,920 |

`sql_exchange` remains linked because retained parser state still uses it for
other host-file clauses, but `select_export` and `select_dump` method bodies
are no longer defined in the linked smoke binary.

## License, Trademark, And Dependency Impact

No new dependency or license change.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh
git diff --check
```

Measure:

```sh
llvm-nm --demangle --size-sort --print-size --radix=d \
  build/mariadb-minsize-no-select-outfile/mylite/mylite-open-close-smoke |
  rg 'select_export|select_dump|sql_exchange'
```

Verification completed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-outfile \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

## Acceptance Criteria

- `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` report
  `ER_NOT_SUPPORTED_YET` in the minsize profile.
- `SELECT ... INTO @var` still works.
- Open/close, storage-engine, embedded-bootstrap, and compatibility smokes
  pass.
- Linked smoke no longer defines `select_export` or `select_dump` method
  bodies.
- Size deltas are recorded in this spec and production size analysis.

## Risks And Unresolved Questions

- This is a visible SQL compatibility tradeoff. It belongs only in the
  aggressive size profile unless product compatibility accepts removing
  server-side file export.
- `sql_exchange` is retained for now because `LOAD DATA` parser state still
  constructs it. A later parser-maintenance slice can revisit that shared type.
