# Ownerless Foreign-Key Mixed Column Mutation Live Recovery

## Problem Statement

Ownerless live-peer recovery already covers FK-only mixed ADD/DROP ALTER lists
and focused mixed FK plus non-FK `ADD COLUMN`, table `COMMENT`, and column
default ALTER lists. The remaining mixed FK plus non-FK matrix still includes
column mutation lists that combine foreign-key metadata removal, foreign-key
metadata creation, and a real column definition change in one successful
MariaDB `ALTER TABLE`.

This slice covers exact mixed ALTER lists with one `DROP FOREIGN KEY`, one
real column `DROP`, `MODIFY`, `CHANGE`, or `RENAME COLUMN` clause, and one
`ADD CONSTRAINT ... FOREIGN KEY ... REFERENCES ...` clause. A dead writer can
be recovered while another ownerless peer remains live, and the native
file-operation checkpoint marker remains durable until final no-live drain.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:7965-7974` parses comma-separated ALTER list items.
- `mariadb/sql/sql_yacc.yy:8014-8025` parses `CHANGE` and `MODIFY COLUMN`
  items into `Alter_info` column-change flags.
- `mariadb/sql/sql_yacc.yy:8051` parses `DROP FOREIGN KEY`, and
  `mariadb/sql/sql_yacc.yy:8122-8124` parses `RENAME COLUMN old TO new`.
- `mariadb/sql/sql_table.cc:7052-7241` maps parser ALTER flags to handler
  column-change and column-name handler flags.
- `mariadb/sql/sql_table.cc:9709-10057` validates column changes against
  foreign-key metadata and processes FK additions/removals.
- `mariadb/storage/innobase/handler/handler0alter.cc:125-126` classifies FK
  ADD/DROP as native InnoDB FK ALTER operations, and
  `mariadb/storage/innobase/handler/handler0alter.cc:11294-11758` commits the
  native inplace ALTER path.

## Scope And Non-Goals

In scope:

- Exact schema-qualified mixed ALTER lists over non-generated InnoDB child
  tables.
- One existing FK DROP clause, one new FK ADD clause, and one of:
  - `DROP [COLUMN] note`,
  - `MODIFY [COLUMN] note <plain definition>`,
  - `CHANGE [COLUMN] note changed_note <plain definition>`,
  - `RENAME COLUMN note TO renamed_note`.
- Live-peer dictionary recovery at `dictionary-before-finish`.
- Native file-operation marker retention while a peer remains live and no-live
  drain after the peer exits.
- Recovered FK metadata/enforcement, column metadata, ownerless/native reopen,
  and forced `.shm` rebuild.

Out of scope:

- Arbitrary multi-clause ALTER parsing.
- More than one non-FK column mutation in the same mixed FK list.
- Generated-column tables, column placement, indexes, CHECK clauses,
  `AUTO_INCREMENT`, explicit online option matrices, partition/table-admin
  paths, and tablespace detach/import.
- SQL-level table-lock callback reachability.
- Randomized external MariaDB/RQG stress.

## Design

The mixed-FK recovery classifier now returns the existing column recovery kind
for a bounded mixed list instead of only reporting whether it saw `ADD COLUMN`.
FK-only mixed lists still use the metadata-only FK recovery kind. Mixed lists
with one accepted column mutation use the corresponding existing column
recovery kind so the native file-operation marker is forced before dictionary
finish and retained until no-live checkpoint drain.

Clause-level recognizers mirror the existing single-clause column guards:

- `DROP COLUMN` proves the source column exists and the table has no generated
  columns.
- `MODIFY COLUMN` and `CHANGE COLUMN` accept only plain stored-column
  definitions without generated, placement, key, FK, CHECK, or explicit
  algorithm/lock tokens, and prove source/target column metadata before
  execution.
- `RENAME COLUMN` proves the old column exists and the new column is absent.

The implementation stays in MyLite first-party SQL classification and test
code. MariaDB remains authoritative for SQL parsing, native dictionary writes,
and InnoDB storage effects.

## Compatibility Impact

No new SQL syntax or behavior is enabled. The slice broadens ownerless crash
classification for MariaDB-compatible ALTER statements that have already
succeeded natively. Unsupported shapes remain conservative and unclaimed.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or durable shared-state format changes. Durable database
state remains inside the MyLite-owned database directory. The slice reuses the
existing ownerless dictionary recovery record and native file-operation marker
drain policy.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The diff adds bounded classifier branches, four hook selectors, CTest
registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the direct selectors:
  - `dictionary-foreign-key-mixed-drop-column-alter-crash`
  - `dictionary-foreign-key-mixed-modify-column-alter-crash`
  - `dictionary-foreign-key-mixed-change-column-alter-crash`
  - `dictionary-foreign-key-mixed-rename-column-alter-crash`
- Run the registered CTests, repeated under hook build.
- Run adjacent FK mixed crash selectors.
- Run production ownerless selectors, DDL stress, production build guards,
  format check, and `git diff --check`.

## Acceptance Criteria

- Each selector reaches `dictionary-before-finish` after native ALTER success.
- Live-peer recovery observes the recovered FK metadata and column state while
  the native file-operation marker remains set.
- The marker drains after the last live peer exits.
- Ownerless reopen, ordinary native reopen, and forced shared-memory rebuild
  observe the same recovered rows, FK enforcement, column metadata, and indexes.
- Existing FK-only and mixed FK ADD/comment/default behavior remains unchanged.

## Risks And Follow-Up

- The classifier is intentionally exact-shape, not a MariaDB ALTER parser.
- Broader FK/non-FK ALTER lists with multiple non-FK clauses, generated-column
  tables, index/CHECK/rebuild combinations, broader DDL file lifecycle,
  rollback internals, active-reader crash breadth, and external randomized
  oracle stress remain ownerless concurrency completion work.
