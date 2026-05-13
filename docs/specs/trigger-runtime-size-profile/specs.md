# Trigger Runtime Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's full trigger
runtime. MyLite already rejects `CREATE TRIGGER` and `DROP TRIGGER` in embedded
command dispatch because MariaDB triggers are backed by `.TRG` trigger
definition files and `.TRN` trigger-name files. Retaining file-backed trigger
loading and execution keeps unsupported sidecar metadata code in the embedded
library.

Current baseline after `xa-transaction-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,920,080 |
| `sql_trigger.cc.o` object | 83,856 |
| stripped `mylite-open-close-smoke` | 5,719,056 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/schema-object-ddl-rejection/specs.md` documents embedded
  rejection for `CREATE TRIGGER` and `DROP TRIGGER`.
- `vendor/mariadb/server/sql/sql_parse.cc` rejects `SQLCOM_CREATE_TRIGGER`
  and `SQLCOM_DROP_TRIGGER` under `EMBEDDED_LIBRARY` before calling
  `mysql_create_or_drop_trigger()`.
- `vendor/mariadb/server/sql/sql_trigger.cc` defines `.TRG` and `.TRN` file
  extensions, parses trigger definition files in
  `Table_triggers_list::check_n_load()`, executes loaded triggers in
  `Table_triggers_list::process_triggers()`, and updates sidecar files during
  drop and rename paths.
- `vendor/mariadb/server/sql/sql_base.cc` calls
  `Table_triggers_list::check_n_load()` while opening tables. If no trigger
  metadata is loaded, `TABLE::triggers` remains null and ordinary DML paths
  treat the table as having no triggers.
- `vendor/mariadb/server/sql/sql_table.cc`, `sql_rename.cc`, and
  `ddl_log.cc` call trigger drop/rename helpers during table DDL cleanup and
  recovery. Those helpers must remain as inert link stubs in the minsize
  profile.
- `vendor/mariadb/server/sql/sql_show.cc` uses `load_table_name_for_trigger()`
  and `Table_triggers_list::find_trigger()` for `SHOW CREATE TRIGGER`.
  With no trigger sidecar support, this command should report the trigger as
  absent instead of trying to parse `.TRN` and `.TRG` files.

## Scope

Add a minsize option that removes the file-backed trigger runtime from the
embedded library. The option will:

- remove `../sql/sql_trigger.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned trigger runtime stub;
- keep `mysql_create_or_drop_trigger()` rejecting defensively even though
  embedded command dispatch already rejects trigger DDL;
- make `Table_triggers_list::check_n_load()` return success without loading
  sidecar trigger metadata;
- make trigger drop, rename, execution, and prelocking helpers inert; and
- keep `SHOW CREATE TRIGGER` and information-schema trigger discovery from
  loading external sidecar files.

## Non-Goals

- Do not implement MyLite trigger metadata storage.
- Do not emulate `.TRG` or `.TRN` files in the `.mylite` file.
- Do not change non-embedded MariaDB behavior.
- Do not remove trigger parser syntax in this slice.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_TRIGGER_RUNTIME` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_trigger_stub.cc`. The stub will
define the trigger symbols needed by existing SQL, DDL, and information-schema
call sites. The default behavior is:

- explicit trigger DDL returns MariaDB's embedded-disabled diagnostic;
- trigger loading reports success while leaving `TABLE::triggers` null;
- trigger execution and metadata-update methods return success/no-op because
  no triggers can be loaded;
- name lookup returns no trigger; and
- `load_table_name_for_trigger()` reports missing trigger metadata, so
  `SHOW CREATE TRIGGER` remains an absent-trigger path rather than a sidecar
  file read.

## Affected Subsystems

- Embedded minsize SQL source list.
- Table open trigger sidecar discovery.
- Trigger DDL fallback symbol.
- Table drop/rename cleanup hooks.
- Information-schema and `SHOW CREATE TRIGGER` trigger metadata paths.
- Binary-size documentation.

## DDL Metadata Routing Impact

This slice reinforces the existing routing decision that trigger metadata is
unsupported until MyLite owns it. It removes MariaDB's `.TRG` and `.TRN`
sidecar implementation from the aggressive embedded profile instead of
redirecting those sidecars into the MyLite catalog.

## Single-File And Embedded-Lifecycle Impact

This removes a persistent sidecar metadata surface from the embedded runtime.
It does not create MyLite companion files and does not change `.mylite` file
ownership.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are bounded by the 83,856-byte `sql_trigger.cc.o`
member minus the replacement stub. Linked-runtime savings may be smaller
because `sql_show.cc` still defines `show_create_trigger()`, but the full
trigger loader, execution, sidecar-file parser, and trigger object methods
should no longer be retained.

Measured result on top of `xa-transaction-size-profile`:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,848,586 | -71,494 |
| `mylite_trigger_stub.cc.o` object | 15,112 | replaces 83,856-byte `sql_trigger.cc.o` |
| `mylite/mylite-open-close-smoke` | 7,932,008 | -19,576 |
| stripped `mylite-open-close-smoke` | 5,705,280 | -13,776 |

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `sql_trigger.cc.o` in `libmariadbd.a`;
- presence and size of the replacement stub;
- retained linked trigger symbols; and
- trigger DDL unsupported diagnostics from embedded bootstrap smoke.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-trigger-runtime \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

The archive contains `mylite_trigger_stub.cc.o` and no longer contains
`sql_trigger.cc.o`. Embedded bootstrap still reports embedded-disabled
diagnostics for `CREATE TRIGGER` and `DROP TRIGGER`. The compatibility harness
reports `status=0` for all groups, including the sidecar scan. The build tree
contains no `.TRG` or `.TRN` files after the harness run.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- Embedded bootstrap still verifies trigger DDL is explicitly unsupported.
- The embedded archive no longer contains `sql_trigger.cc.o`.
- Ordinary table create, drop, rename, DML, and information-schema smokes do
  not regress.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- This removes a real MariaDB SQL feature, not just daemon plumbing. It belongs
  only in the aggressive embedded-size profile.
- Future MyLite trigger support must disable this option and design catalog
  storage, execution lifecycle, recovery, and compatibility tests.
- `SHOW CREATE TRIGGER` remains parser-visible, but with trigger sidecar
  loading disabled it should only report missing trigger metadata.
