# Foreign-Key Create Info

## Goal

Expose MyLite-owned foreign-key metadata in `SHOW CREATE TABLE` output for
manually seeded internal FK records, without enabling public FK DDL or row
enforcement. This completes the read-only MariaDB metadata surface needed before
later DDL publication and enforcement slices can safely accept FK SQL.

## Non-Goals

- Accepting `CREATE TABLE` or `ALTER TABLE` foreign-key DDL through
  `libmylite`.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS`.
- Enforcing child/parent row existence checks, restrict checks, cascading
  actions, or `foreign_key_checks=0` import semantics.
- Implementing FK-aware index drop/change checks.
- Implementing statement-scoped table-plus-FK publication.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_show.cc:show_create_table()` appends the string returned by
  `handler::get_foreign_key_create_info()` directly into the table definition
  before table-level CHECK constraints.
- `mariadb/sql/handler.h:handler::get_foreign_key_create_info()` returns an
  allocated FK clause string and `free_foreign_key_create_info()` owns cleanup.
- `mariadb/storage/innobase/dict/dict0dict.cc:
  dict_print_info_on_foreign_key_in_create_format()` emits comma-prefixed FK
  clauses of the form used by `SHOW CREATE TABLE`.
- `mariadb/sql/sql_show.h:append_identifier()` is the server helper for
  quoting identifiers according to the active SQL mode.

## Compatibility Impact

Public FK SQL compatibility remains unchanged: direct and prepared FK DDL still
reject before MariaDB execution, including routed `ENGINE=InnoDB` tables. This
slice only affects `SHOW CREATE TABLE` for internal FK records seeded through
MyLite storage primitives. The compatibility matrix must continue to mark
foreign keys partial and state that FK DDL and enforcement remain planned.

## Design

Implement `ha_mylite::get_foreign_key_create_info()` in the MyLite handler. The
method lists child FK metadata for `storage_schema()` / `storage_table()` and
formats each record as a comma-prefixed table constraint:

```sql
,
  CONSTRAINT `fk_name` FOREIGN KEY (`child_col`)
  REFERENCES `parent_table` (`parent_col`) ON DELETE ... ON UPDATE ...
```

When the referenced schema differs from the child schema, the referenced table
is schema-qualified. Identifiers are formatted with MariaDB's
`append_identifier()` helper, not hand-quoted. `RESTRICT` and unspecified
actions are omitted to match InnoDB `SHOW CREATE TABLE` formatting; `CASCADE`,
`SET NULL`, `NO ACTION`, and `SET DEFAULT` are emitted when present.

Return `NULL` when there are no stored child FK records or when the table uses
volatile rows. Return an allocated string for non-empty FK output and release it
from `ha_mylite::free_foreign_key_create_info()`.

## File Lifecycle

No new files are introduced. The hook reads existing FK catalog records and
typed FK blob pages from the primary `.mylite` file. No `.frm`, `.ibd`, `.MYD`,
`.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or plugin-owned
durable file is introduced.

## Embedded Lifecycle And API

No public `libmylite` API is added. Open/close ownership is unchanged. The hook
uses the active embedded runtime's primary file path and remains a read-only
metadata operation.

## Build, Size, And Dependencies

No dependency is introduced. The MariaDB fork delta stays inside
`mariadb/storage/mylite/`. Binary-size impact is expected to be small and
limited to formatting helpers.

## Test Plan

- Storage-smoke coverage that manually seeded FK metadata appears in
  `SHOW CREATE TABLE`.
- Storage-smoke coverage that public FK DDL still rejects before catalog
  publication after `SHOW CREATE TABLE` support is added.
- Format check, storage-smoke build/test when handler code changes, full
  default `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- `SHOW CREATE TABLE` includes MyLite-owned child FK clauses for manually
  seeded internal metadata.
- Referenced tables in other schemas are schema-qualified.
- Identifier quoting follows MariaDB SQL mode behavior through
  `append_identifier()`.
- Public FK SQL remains rejected and `HTON_SUPPORTS_FOREIGN_KEYS` remains
  clear.
- FK metadata remains single-file and does not rely on InnoDB dictionary state
  or persistent MariaDB sidecars.

## Risks And Open Questions

- `SHOW CREATE TABLE` now exposes seeded internal FK metadata before public FK
  DDL exists. Docs and tests must keep that distinction explicit.
- `SET DEFAULT` formatting is included for storage completeness, but MariaDB FK
  behavior around this action may need a stricter compatibility decision before
  public DDL support.
