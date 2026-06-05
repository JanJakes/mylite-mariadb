# Ownerless Unique Index Idempotent DDL

## Problem

Ownerless unique-index coverage proved create, replacement, drop, and duplicate
enforcement across already-open peers. MariaDB also supports idempotent
`CREATE UNIQUE INDEX IF NOT EXISTS` and `ALTER TABLE ... ADD UNIQUE INDEX IF
NOT EXISTS` spellings. MyLite needs focused evidence that ownerless dictionary
refresh preserves the existing unique definition for those no-op branches, and
that ordinary duplicate index-name errors remain visible to the peer process.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level
  `CREATE [OR REPLACE] UNIQUE INDEX opt_if_not_exists` through
  `Lex->add_create_index(Key::UNIQUE, ...)`, carrying the create-or-replace
  and idempotency bits into the alter-table path.
- `mariadb/sql/sql_yacc.yy:key_def` parses table-element unique-key
  definitions through `constraint_key_type opt_if_not_exists` and
  `Lex->add_key()`, covering `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS`.
- `mariadb/sql/sql_yacc.yy:alter_list_item` parses
  `DROP key_or_index opt_if_exists_table_element` into an `Alter_drop` with the
  `IF EXISTS` bit.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` operations before native execution, pushes
  `ER_DUP_KEYNAME` as a note, and preserves the current key definition. The same
  loop handles unique keys because it iterates `alter_info->key_list`.

## Scope And Non-Goals

- Extend the existing `unique-index-ddl` selector rather than add another
  ownerless SQL shard entry.
- Cover top-level `CREATE UNIQUE INDEX IF NOT EXISTS` create/no-op behavior.
- Verify plain duplicate `CREATE UNIQUE INDEX` and duplicate
  `ALTER TABLE ... ADD UNIQUE INDEX` return MariaDB errno 1061.
- Cover `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` create/no-op behavior.
- Cover missing and repeated `ALTER TABLE ... DROP INDEX IF EXISTS` behavior.
- Preserve the existing final row totals and absent-index reopen checks.
- Do not add primary-key idempotency, special full-text/spatial index
  idempotency, algorithm/lock option matrices, or crash injection for these
  no-op unique-index branches.

## Design

The child ownerless process continues to own all unique-index DDL boundaries for
`app.ownerless_unique_index_base`. It now creates the original
`(tenant_id, slug)` unique key with `IF NOT EXISTS`, repeats the same index name
over `(tenant_id, weight)` with `IF NOT EXISTS`, replaces it with
`CREATE OR REPLACE UNIQUE INDEX`, drops it, recreates it with
`ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS`, repeats that ALTER add over
the duplicate-prone old key definition, runs a missing ALTER drop, and finally
runs a repeated real ALTER drop.

The parent keeps an ownerless handle open across every boundary. It verifies
`information_schema.statistics.NON_UNIQUE = 0`, key-part order, forced-index
reads, duplicate-key-name errno 1061, duplicate-key errno 1062, and final index
absence. The no-op phases intentionally request a different key definition than
the existing one, so preserving the original key part and duplicate enforcement
is observable.

## Compatibility Impact

No new SQL surface is enabled. This expands ownerless compatibility evidence for
MariaDB unique secondary-index DDL to include idempotent top-level and
`ALTER TABLE` spellings, while preserving the existing partial status for broad
index option coverage.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB dictionary
metadata inside the MyLite database directory and reuses the existing
ownerless/native reopen plus forced `.shm` rebuild checks.

## Native Storage Impact

No storage format changes. Mutating phases create/drop native InnoDB unique
secondary indexes; idempotent no-op phases must leave the native key definition
unchanged.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `unique-index-ddl` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The original `CREATE UNIQUE INDEX IF NOT EXISTS` produces a peer-visible
  multi-column unique index.
- Plain duplicate unique-index create returns errno 1061.
- Duplicate top-level `CREATE UNIQUE INDEX IF NOT EXISTS` preserves the
  original key definition and duplicate-key enforcement.
- `CREATE OR REPLACE UNIQUE INDEX` still moves enforcement to the replacement
  key definition.
- `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` creates the replacement
  unique index after drop.
- Duplicate plain ALTER add returns errno 1061.
- Duplicate ALTER add with `IF NOT EXISTS` preserves the existing unique key
  definition.
- Missing and repeated ALTER drops leave the expected present/absent metadata.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Inline `CREATE TABLE` index idempotency, randomized DDL oracle coverage, and
  crash-injected idempotent unique-index no-op branches remain planned broader
  work. Primary-key ADD idempotency is covered by
  `ownerless-primary-key-idempotent-ddl`; idempotent FULLTEXT/SPATIAL policy
  rejection is covered by `ownerless-special-index-idempotent-policy`.
