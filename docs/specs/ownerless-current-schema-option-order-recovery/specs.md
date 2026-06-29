# Ownerless Current-Schema Option-Order Recovery

## Problem

Ownerless schema metadata crash recovery already covers named
`ALTER DATABASE <schema> COMMENT ... DEFAULT CHARACTER SET ... COLLATE ...`
option-order rewrites, current-schema default rewrites, and current-schema
comment rewrites. The remaining evidence gap is the composition: a killed
current-schema `ALTER DATABASE` that changes both the schema comment and the
default charset/collation after MariaDB rewrites native `db.opt` but before
ownerless dictionary finish.

This slice should prove that boundary without changing supported SQL
semantics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` resolves `ALTER DATABASE ident_or_empty` through
  `copy_db_to(&lex->name)` when the identifier is omitted, so current-schema
  `ALTER DATABASE` uses the same `SQLCOM_ALTER_DB` command after grammar
  resolution.
- `mariadb/sql/sql_yacc.yy` accepts `COMMENT`, `DEFAULT CHARACTER SET`, and
  `COLLATE` in `create_database_options`, including the current-schema
  `ALTER DATABASE COMMENT ... opt_create_database_options` spelling.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_ALTER_DB` by resolving charset and
  collation through `resolve_to_charset_collation_context()` and then calling
  `mysql_alter_db()`.
- `mariadb/sql/sql_db.cc` writes `db.opt` through `write_db_opt()`, preserving
  missing existing values for `ALTER DATABASE` and writing default charset,
  default collation, and optional comment into the schema metadata file.

## Design

Add a hook-build crash test for:

```sql
USE ownerless_schema_current_option_order_crash;
ALTER DATABASE COMMENT = 'ownerless current recovered option order'
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
```

The writer is killed at the existing `dictionary-before-finish` fault after
MariaDB has rewritten `db.opt`. A live ownerless peer remains open. Reopen must
recover the schema comment and defaults, preserve the pre-alter table's
latin1 column metadata, keep the native file-operation checkpoint marker clear,
allow a post-recovery table to inherit utf8mb4 defaults, and survive
ownerless/native reopen plus forced `.shm` rebuild.

No production-code change is planned unless this coverage exposes a recovery
bug.

## Compatibility Impact

No public SQL, C API, wire protocol, or directory-layout behavior changes. This
narrows the ownerless schema DDL recovery evidence gap for MariaDB-compatible
current-schema `ALTER DATABASE` syntax.

## Storage And Lifecycle Impact

The test exercises native `datadir/<schema>/db.opt` metadata inside the
MyLite-owned database directory. The ownerless durable native file-operation
marker must remain clear because this is a metadata-only schema option rewrite,
not a table file lifecycle operation.

## Tests And Verification

- Add a direct unsafe-hook selector:
  `dictionary-current-schema-option-order-crash`.
- Add a focused CTest:
  `libmylite.ownerless-dictionary-current-schema-option-order-crash`.
- Run the focused hook selector, relevant schema DDL hook subset, formatting,
  production-build audit, and whitespace check.

## Acceptance Criteria

- The killed current-schema option-order writer recovers while a peer remains
  live.
- Recovered `INFORMATION_SCHEMA.SCHEMATA` shows `utf8mb4`,
  `utf8mb4_unicode_ci`, and the new comment.
- The pre-existing table keeps latin1 column metadata.
- A post-recovery table inherits utf8mb4 column metadata.
- Ownerless and ordinary native reopen succeed before and after forced shared
  memory rebuild.
- The native file-operation checkpoint marker remains clear throughout the
  metadata-only boundary.

## Non-Goals

- Exhaustive schema option permutations.
- SQL-level ownerless table-wait reachability.
- External MariaDB/RQG randomized stress.
- New production recovery policy.
