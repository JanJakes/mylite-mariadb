# Select Procedure Runtime Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's generic
`SELECT ... PROCEDURE` dispatch framework in `procedure.cc` even after the
only built-in procedure, `PROCEDURE ANALYSE()`, is already replaced with an
unsupported MyLite stub.

MariaDB documents the `PROCEDURE` SELECT clause as a C procedure hook and says
`ANALYSE` is the only available procedure. That makes the remaining dispatch
framework dead weight in MyLite's embedded runtime.

Current baseline after `persistent-statistics-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,621,550 |
| `procedure.cc.o` object | 111,288 |
| `mylite_procedure_analyse_stub.cc.o` object | 1,960 |
| stripped `mylite-open-close-smoke` | 5,641,032 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for the `PROCEDURE` SELECT clause says the clause
  sends result rows through a C procedure and that `ANALYSE` is currently the
  only available procedure:
  <https://mariadb.com/docs/server/reference/sql-statements/data-manipulation/selecting-data/procedure>.
- `docs/specs/procedure-analyse-size-profile/specs.md` already removes
  `sql_analyse.cc` and verifies `SELECT ... PROCEDURE ANALYSE()` returns an
  unsupported-feature diagnostic in the minsize profile.
- `vendor/mariadb/server/sql/sql_yacc.yy` still parses
  `PROCEDURE_SYM ident` after `SELECT` and stores the clause in
  `LEX::proc_list`.
- `vendor/mariadb/server/sql/sql_select.cc` calls
  `setup_procedure()` during SELECT preparation and blocks incompatible
  procedure combinations after a procedure object is returned.
- `vendor/mariadb/server/sql/procedure.cc` defines the generic dispatch table,
  `Item_proc_*` output helper items, and `setup_procedure()`.
- Symbol intersection against the current archive shows only
  `setup_procedure(THD*, ORDER*, select_result*, List<Item>&, int*)` is needed
  externally from `procedure.cc.o`.

## Scope

Add a minsize option that removes the generic SELECT procedure runtime from the
embedded library. The option will:

- remove `../sql/procedure.cc` from `SQL_EMBEDDED_SOURCES`;
- remove `mylite_procedure_analyse_stub.cc` when the whole SELECT procedure
  runtime is disabled;
- add a MyLite-owned `setup_procedure()` stub; and
- preserve a clear unsupported-feature diagnostic for
  `SELECT ... PROCEDURE ANALYSE()`.

## Non-Goals

- Do not remove stored procedures or stored functions in this slice.
- Do not edit the generated parser or remove `PROCEDURE` grammar.
- Do not change non-embedded MariaDB behavior.
- Do not remove ordinary SELECT execution, result metadata, grouping, or
  ordering behavior.

## Proposed Design

Add `MYLITE_DISABLE_SELECT_PROCEDURE_RUNTIME` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_select_procedure_stub.cc`.
The stub defines `setup_procedure()`. If no `PROCEDURE` clause is present, it
returns `nullptr` with `*error = 0`, matching MariaDB's no-procedure behavior.
If a clause is present, it raises `ER_NOT_SUPPORTED_YET`, sets `*error = 1`,
and returns `nullptr`.

The stub intentionally does not include `procedure.h`, because that header
defines `Item_proc_*` helper classes that belong to the removed runtime. It
uses forward declarations plus the retained SQL headers needed for the exact
`setup_procedure()` signature.

## Affected Subsystems

- Embedded minsize SQL source list.
- SELECT procedure-clause setup.
- `libmylite` open/close smoke expectation for `PROCEDURE ANALYSE()`.
- Binary-size documentation.

## DDL Metadata Routing Impact

None. The `PROCEDURE` SELECT clause is query post-processing, not schema
metadata. The smoke creates a normal MyLite table only to reach the SELECT
path.

## Single-File And Embedded-Lifecycle Impact

The slice removes an in-process result-post-processing hook. It does not add
files, sidecars, locks, or lifecycle state.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

SQL compatibility impact: the aggressive minsize profile no longer supports
the generic `SELECT ... PROCEDURE` clause. `PROCEDURE ANALYSE()` was already
unsupported before this slice.

## Binary-Size Impact

Expected archive savings are roughly 113 KiB from removing `procedure.cc.o`
and the now-unreachable `mylite_procedure_analyse_stub.cc.o`, minus a small
replacement stub. Linked-runtime savings may be smaller because SELECT
preparation remains live.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `procedure.cc.o` and `mylite_procedure_analyse_stub.cc.o` in
  `libmariadbd.a`;
- presence and size of the replacement stub; and
- compatibility-harness status.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- `SELECT ... PROCEDURE ANALYSE()` still fails explicitly in the minsize
  profile.
- The embedded archive no longer contains `procedure.cc.o`.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Implementation Results

Implemented in the minsize profile:

- `MYLITE_DISABLE_SELECT_PROCEDURE_RUNTIME=ON`;
- `../sql/procedure.cc` omitted from `SQL_EMBEDDED_SOURCES`;
- `mylite_procedure_analyse_stub.cc` omitted when the full procedure runtime
  is disabled; and
- `mylite_select_procedure_stub.cc` provides the retained
  `setup_procedure()` symbol and reports `ER_NOT_SUPPORTED_YET` for any
  parsed `SELECT ... PROCEDURE` clause.

Measured from
`MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,500,552 |
| `mylite/libmylite.a` | 122,800 |
| `storage/mylite/libmylite_embedded.a` | 388,440 |
| `mylite/mylite-open-close-smoke` | 7,848,088 |
| stripped `mylite-open-close-smoke` | 5,640,696 |
| `mylite/mylite-compatibility-smoke` | 7,722,376 |
| stripped `mylite-compatibility-smoke` | 5,535,488 |
| `mylite_select_procedure_stub.cc.o` | 1,792 |

Compared with the persistent-statistics baseline, this saves 120,998 bytes in
`libmariadbd.a`, removes one archive object, and saves 336 bytes in the
stripped linked open/close smoke. The linked-runtime win is tiny because
ordinary SELECT setup still remains live; the main value is archive cleanup and
removing an embedded-hostile extension hook after its only built-in procedure
is already unsupported.

The archive contains `mylite_select_procedure_stub.cc.o` and no longer contains
`procedure.cc.o` or `mylite_procedure_analyse_stub.cc.o`.

Verification passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-select-procedure \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

## Risks And Unresolved Questions

- Parser syntax remains available, so unsupported `PROCEDURE` clauses fail
  during SELECT procedure setup rather than parse time.
- This removes the generic SELECT procedure extension hook, not just the
  `ANALYSE` implementation. That is acceptable for the aggressive embedded
  size profile because MariaDB exposes no other built-in procedures.
