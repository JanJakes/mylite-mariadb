# Ownerless Schema Charset Alias Order Recovery

## Problem

Ownerless schema crash coverage proves named/current-schema default rewrites,
comment rewrites, and combined comment-first `ALTER DATABASE ... DEFAULT
CHARACTER SET ... COLLATE ...` rewrites. The remaining schema-option gap still
includes accepted MariaDB spellings where schema options appear in a different
order or use the `CHARSET` alias.

This slice adds focused live-peer recovery evidence for a killed schema
`db.opt` rewrite that supplies collation before charset and uses `CHARSET`
instead of `CHARACTER SET`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5629` parses schema options as a repeated
  `create_database_options` list, so accepted options can appear in different
  orders.
- `mariadb/sql/sql_yacc.yy:5634` accepts `default_collation`,
  `default_charset`, and `COMMENT` as schema options.
- `mariadb/sql/sql_yacc.yy:6003` accepts optional `DEFAULT` before `COLLATE`,
  and `mariadb/sql/sql_yacc.yy:6908` accepts both `CHAR SET` and `CHARSET` for
  charset clauses.
- `mariadb/sql/sql_db.cc` writes schema defaults and comments into the native
  schema `db.opt` file used by `INFORMATION_SCHEMA.SCHEMATA`.

## Design

Add the unsafe-hook selector
`dictionary-schema-charset-alias-order-crash`.

The test creates `ownerless_schema_charset_alias_order_crash` with `latin1`
defaults and an initial comment, then creates a pre-alter InnoDB table. A child
writer runs:

```sql
ALTER DATABASE ownerless_schema_charset_alias_order_crash
  DEFAULT COLLATE utf8mb4_unicode_ci
  CHARSET utf8mb4
  COMMENT = 'ownerless recovered charset alias order'
```

with the existing `dictionary-before-finish` fault armed. The parent keeps a
second ownerless peer live, verifies recovery with the native file-operation
marker clear, releases the peer, creates a post-recovery table, and verifies
ownerless/native reopen before and after forced shared-memory rebuild.

## Compatibility Impact

No SQL or API behavior changes. The slice narrows the ownerless schema DDL
recovery evidence gap for MariaDB-compatible schema option spellings.

## Storage And Lifecycle Impact

The covered native file is `datadir/<schema>/db.opt` inside the MyLite database
directory. This is a metadata-only schema rewrite, so the native file-operation
checkpoint marker must remain clear.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-schema-(charset-alias-order|option-order|comment)-crash$' --output-on-failure`.
- Run adjacent schema hook selectors as needed.
- Run the relevant production embedded schema/default/comment subset.
- Run format and whitespace checks.

## Acceptance Criteria

- A killed collation-first `CHARSET` alias schema rewrite recovers while an
  ownerless peer remains live.
- Recovered `INFORMATION_SCHEMA.SCHEMATA` shows the new comment,
  `utf8mb4`, and `utf8mb4_unicode_ci`.
- The pre-existing table keeps `latin1_swedish_ci`.
- A table created after recovery inherits `utf8mb4_unicode_ci`.
- Ownerless and ordinary native reopen pass before and after forced `.shm`
  rebuild.

## Non-Goals

- Exhaustive schema option permutations.
- Invalid schema-option cleanup.
- SQL-level table-wait reachability.
- External MariaDB/RQG randomized stress.
