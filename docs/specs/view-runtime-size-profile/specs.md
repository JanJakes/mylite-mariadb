# View Runtime Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's full view
runtime. MyLite already rejects `CREATE VIEW`, `ALTER VIEW`, and `DROP VIEW`
in embedded command dispatch because MariaDB views are stored as `.frm` view
definition files. Retaining the full view loader, parser, registration, repair,
and rename implementation keeps unsupported schema-object sidecar code in the
embedded library.

Current baseline after `trigger-runtime-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,848,586 |
| `sql_view.cc.o` object | 47,296 |
| stripped `mylite-open-close-smoke` | 5,705,280 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/schema-object-ddl-rejection/specs.md` documents embedded
  rejection for `CREATE VIEW`, `ALTER VIEW`, and `DROP VIEW`.
- `vendor/mariadb/server/sql/sql_parse.cc` rejects `SQLCOM_CREATE_VIEW` and
  `SQLCOM_DROP_VIEW` under `EMBEDDED_LIBRARY` before calling
  `mysql_create_view()` or `mysql_drop_view()`.
- `vendor/mariadb/server/sql/sql_view.cc` implements view `.frm`
  registration, parsing, open-time expansion through `mysql_make_view()`,
  view checksum/repair, and view rename.
- `vendor/mariadb/server/sql/table.cc` can identify `TYPE=VIEW` definitions
  and calls `mariadb_view_version_get()` when a view `.frm` is opened.
- `vendor/mariadb/server/sql/sql_base.cc` calls `mysql_make_view()` when a
  table share is a view. The minsize profile should fail that path rather than
  parsing external view sidecar definitions.
- `vendor/mariadb/server/sql/sql_view.cc` also defines
  `check_duplicate_names()` and `make_valid_column_names()`, which are reused
  by derived table, CTE, and UNION code in `sql_derived.cc`, `sql_cte.cc`, and
  `sql_union.cc`. Those helpers are not view-only and must be preserved.

## Scope

Add a minsize option that removes the file-backed view runtime from the
embedded library. The option will:

- remove `../sql/sql_view.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned view runtime stub;
- keep generic derived-table/CTE column-name validation helpers working;
- keep explicit view DDL rejected defensively;
- make view open/repair/check/rename entry points unsupported or inert; and
- keep ordinary base-table, derived-table, CTE, UNION, and information-schema
  behavior unchanged.

## Non-Goals

- Do not implement MyLite view metadata storage.
- Do not emulate view `.frm` files in the `.mylite` file.
- Do not remove view parser syntax in this slice.
- Do not change non-embedded MariaDB behavior.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_VIEW_RUNTIME` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_view_stub.cc`. The stub will:

- preserve `view_type`;
- preserve the duplicate-name and generated-column-name helpers used outside
  view DDL;
- reject `mysql_create_view()`, `mysql_drop_view()`, and `mysql_make_view()`
  with MariaDB's embedded-disabled diagnostic;
- return no-op success for base-table-only helpers such as
  `check_key_in_view()` and `insert_view_fields()`;
- report `HA_ADMIN_NOT_IMPLEMENTED` for view check/repair/checksum helpers;
  and
- reject view `.frm` version parsing through `mariadb_view_version_get()`.

## Affected Subsystems

- Embedded minsize SQL source list.
- View DDL fallback symbols.
- View `.frm` open/repair/rename hooks.
- Derived-table/CTE/UNION column-name helpers.
- Binary-size documentation.

## DDL Metadata Routing Impact

This slice keeps view metadata unsupported until MyLite owns catalog storage
for it. It removes MariaDB's `.frm` view implementation from the aggressive
embedded profile instead of redirecting view metadata into MyLite storage.

## Single-File And Embedded-Lifecycle Impact

This removes another persistent sidecar metadata surface from the embedded
runtime. It does not create MyLite companion files and does not change
`.mylite` file ownership.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are bounded by the 47,296-byte `sql_view.cc.o`
member minus the replacement stub. Linked savings may be smaller because
view-related `TABLE_LIST`, `Item_direct_view_ref`, information-schema, and
`SHOW CREATE VIEW` paths live in other SQL objects.

Measured result on top of `trigger-runtime-size-profile`:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,810,458 | -38,128 |
| `mylite_view_stub.cc.o` object | 9,776 | replaces 47,296-byte `sql_view.cc.o` |
| `mylite/mylite-open-close-smoke` | 7,916,352 | -15,656 |
| stripped `mylite-open-close-smoke` | 5,691,984 | -13,296 |

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `sql_view.cc.o` in `libmariadbd.a`;
- presence and size of the replacement stub;
- retained linked view symbols; and
- view DDL unsupported diagnostics from embedded bootstrap smoke.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-view-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

The archive contains `mylite_view_stub.cc.o` and no longer contains
`sql_view.cc.o`. Embedded bootstrap still reports embedded-disabled
diagnostics for `CREATE VIEW`, `ALTER VIEW`, and `DROP VIEW`. The
compatibility harness reports `status=0` for all groups, including MariaDB
comparison and sidecar scan. The build tree contains no `.frm` files after the
harness run.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- Embedded bootstrap still verifies view DDL is explicitly unsupported.
- Derived-table, CTE, UNION, and MariaDB comparison checks do not regress.
- The embedded archive no longer contains `sql_view.cc.o`.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- This removes a real MariaDB SQL feature, not just daemon plumbing. It belongs
  only in the aggressive embedded-size profile.
- Future MyLite view support must disable this option and design catalog
  storage, dependency tracking, invalidation, recovery, and compatibility
  tests.
- The replacement must preserve non-view callers of the duplicate-name helper;
  otherwise derived tables and CTEs can regress without touching view DDL.
