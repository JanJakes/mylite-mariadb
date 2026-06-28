# Ownerless Column ALTER Live Recovery

## Problem Statement

Ownerless crash coverage kills real `ALTER TABLE ... DROP COLUMN`,
`ALTER TABLE ... MODIFY COLUMN`, `ALTER TABLE ... CHANGE COLUMN`, and
`ALTER TABLE ... RENAME COLUMN` writers after MariaDB completes the native
table-definition change but before MyLite publishes the ownerless dictionary
finish boundary. The bounded stored-column forms, including exact
`ALGORITHM=COPY, LOCK=EXCLUSIVE` ADD/DROP/MODIFY/CHANGE/RENAME spellings, now
prove live-peer dictionary recovery with native file-operation marker retention.

This slice promotes the bounded real column ALTER forms to live-peer dictionary
recovery. A later ownerless opener may finish the dead writer's ownerless
dictionary generation while another ownerless peer remains open, while the
native file-operation checkpoint marker stays durable until the final no-live
drain.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8014` through `mariadb/sql/sql_yacc.yy:8038`
  parses `CHANGE`, `MODIFY`, and `DROP COLUMN` column ALTER clauses into
  `Alter_info::create_list` or `Alter_info::drop_list` and marks the relevant
  parser flags.
- `mariadb/sql/sql_yacc.yy:8122` through `mariadb/sql/sql_yacc.yy:8126`
  parses `RENAME COLUMN old TO new` into `Alter_column` entries.
- `mariadb/sql/sql_table.cc:6395` through `mariadb/sql/sql_table.cc:6530`
  removes missing `IF EXISTS` drop/alter entries while preserving real column
  mutations.
- `mariadb/sql/sql_table.cc:7040` through `mariadb/sql/sql_table.cc:7240`
  maps parser-side ALTER flags to handler flags and detects true column type,
  default, drop, order, and name changes.
- `mariadb/sql/sql_table.cc:8820` through `mariadb/sql/sql_table.cc:8915`
  builds the post-ALTER field list and rewrites dependent expressions for
  column renames.
- `mariadb/storage/innobase/handler/handler0alter.cc:126` through
  `mariadb/storage/innobase/handler/handler0alter.cc:170` defines InnoDB's
  instant/no-rebuild column ALTER classes, including column rename.

## Design

- Add ownerless dictionary recovery kinds for bounded real column DROP, MODIFY,
  and RENAME statements.
- Classify only narrow single-clause SQL forms:
  - `ALTER TABLE <table> DROP [COLUMN] <column> [RESTRICT|CASCADE]`,
  - `ALTER TABLE <table> MODIFY [COLUMN] <column> <definition>`,
  - `ALTER TABLE <table> RENAME COLUMN <old_column> TO <new_column>`.
- Use pre-execution `INFORMATION_SCHEMA.COLUMNS` metadata to prove the source
  column exists. RENAME also proves the target column is absent.
- Keep DROP/MODIFY limited to tables without generated columns and reject
  multi-clause ALTERs, placement clauses, generated-column definitions,
  index/constraint/FK definitions, explicit algorithm/lock clauses, and
  auto-increment changes.
- Allow RENAME to cover the existing dependent-expression selector because
  MariaDB rewrites generated-column and CHECK expressions in the native ALTER
  path.
- Add the new recovery kinds to the native file-operation live-recovery lane,
  not to the metadata-only lane. Recovery can publish the dictionary boundary
  with a live peer, but must leave the native file-operation marker set until
  no-live checkpoint drain.
- Force the native file-operation marker before dictionary finish for the new
  DROP, MODIFY, and RENAME recovery kinds. This keeps metadata-only-looking
  instant/native ALTER variants on the conservative no-live checkpoint-drain
  path after live dictionary recovery.
- Register the real column crash selectors as standalone hook CTests so CI
  reports timing and failures separately from the large ownerless SQL shard.

## Scope And Non-Goals

In scope:

- Focused real-column DROP, MODIFY, and RENAME crash selectors.
- Existing dependent-expression RENAME crash selector.
- Live-peer ownerless recovery with native file-operation marker retention.
- Ownerless/native reopen and forced `.shm` rebuild verification.

Out of scope:

- Multi-clause column ALTERs.
- Generated-column DROP/MODIFY/CHANGE live recovery.
- Placement, explicit online-option, index/constraint/FK, partition, and
  tablespace-detach/import variants.
- SQL-level table-lock fault injection.
- External randomized DDL oracle expansion.

## Compatibility Impact

No SQL syntax is newly enabled. The slice strengthens ownerless concurrency
evidence for MariaDB-compatible real column ALTER forms by proving MyLite can
complete the ownerless dictionary boundary while a peer remains live after
MariaDB has already persisted the native result.

## DDL Metadata Routing Impact

MariaDB remains authoritative for table-definition mutation and dependent
expression rewrites. MyLite records only a conservative recovery kind before
execution, then uses the dead-owner recovery lane to finish the ownerless
dictionary generation after a killed writer.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains inside the
MyLite database directory. Native file-operation markers remain durable while a
peer is live and are cleared only by the existing no-live checkpoint drain.

## Native Storage Impact

The covered ALTERs continue to use MariaDB/InnoDB native storage and table
metadata. MyLite does not reinterpret `.frm`, InnoDB dictionary, generated
column, CHECK, or tablespace state.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run primitive recovery-kind coverage.
- Run focused selectors:
  - `dictionary-column-drop-crash`
  - `dictionary-column-modify-crash`
  - `dictionary-column-rename-crash`
  - `dictionary-column-rename-expression-crash`
- Run registered standalone hook CTests for the real column crash selectors.
- Run adjacent column/default/idempotent crash selectors.
- Run ownerless DDL stress, production-build guards, format check, and diff
  checks.

## Acceptance Criteria

- The focused selectors reach `dictionary-before-finish` and do not hang.
- A later ownerless opener recovers while the original peer remains live.
- The native file-operation marker remains set while the peer is live and
  clears only after no-live drain.
- Recovered DROP, MODIFY, RENAME, and dependent-expression RENAME metadata and
  row behavior match the existing no-live expectations.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same recovered state.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-dictionary-column-(add|drop|modify|rename|rename-expression)-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-column-(add|drop|modify|rename|rename-expression|default|idempotent-add|idempotent-drop|idempotent-modify|idempotent-rename|idempotent-rename-expression|idempotent-change-expression|idempotent-default-set-expression|idempotent-default-drop-expression|idempotent-change|idempotent-default-set|idempotent-default-drop)-crash$' --output-on-failure`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `git diff --check`

Development note:

- The first focused CTest run showed `MODIFY COLUMN` did not naturally set the
  native file-operation marker. The implemented fix makes the new real column
  ALTER recovery kinds force the marker before ownerless dictionary finish.

## Risks And Unresolved Questions

- The classifiers intentionally cover less syntax than MariaDB's full ALTER
  TABLE grammar.
- Broader DDL/file lifecycle classes, transaction crash windows, active-reader
  crash breadth, and external randomized oracle stress remain completion work
  for ownerless concurrency.
