# Oracle function size profile

## Problem

The MyLite minsize profile already omits MariaDB's generated Oracle SQL-mode
parser, but the linked runtime still carries Oracle-only function builders,
schema routing, and helper `Item` implementations. MariaDB documents
`SQL_MODE=ORACLE` as an Oracle compatibility mode and documents aliases such as
`DECODE_ORACLE` and `LPAD_ORACLE` as Oracle-compatible function surfaces
available outside Oracle mode. MyLite has not claimed that compatibility
surface for the default embedded profile.

This slice tests whether the aggressive minsize profile can omit those
Oracle-only function paths after Oracle-mode parsing has already been made
unsupported.

## Source findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB references:

- `https://mariadb.com/docs/server/server-management/variables-and-modes/sql-mode`
  describes SQL modes and notes that `ORACLE` changes compatibility behavior.
- `https://mariadb.com/docs/release-notes/community-server/about/compatibility-and-differences/sql_modeoracle`
  describes `SQL_MODE=ORACLE` as Oracle compatibility syntax and behavior.
- `https://mariadb.com/docs/server/reference/sql-functions/control-flow-functions/decode_oracle`
  documents `DECODE_ORACLE` as the Oracle-mode `DECODE` synonym available in
  all modes.
- `https://mariadb.com/docs/server/reference/sql-functions/string-functions/lpad`
  documents `LPAD_ORACLE` as the Oracle-mode variant available outside Oracle
  mode.

Relevant source paths:

- `vendor/mariadb/server/sql/item_create.cc` registers Oracle aliases such as
  `DECODE_ORACLE`, `LPAD_ORACLE`, `LTRIM_ORACLE`, `REPLACE_ORACLE`,
  `RPAD_ORACLE`, `RTRIM_ORACLE`, `SUBSTR_ORACLE`, and
  `CONCAT_OPERATOR_ORACLE`; it also builds `native_functions_hash_oracle` by
  replacing normal function builders with Oracle variants.
- `vendor/mariadb/server/sql/sql_schema.cc` defines `Schema_oracle`, routes
  implied function lookup to `native_functions_hash_oracle` when
  `MODE_ORACLE` is set, maps `oracle_schema.DATE`, and constructs Oracle
  variants for `REPLACE`, `SUBSTR`, and `TRIM`.
- `vendor/mariadb/server/sql/sql_yacc.yy` still has MariaDB-parser actions that
  instantiate `Item_func_concat_operator_oracle`,
  `Item_func_oracle_sql_rowcount`, and
  `Lex_trim_st::make_item_func_trim_oracle()`.
- `vendor/mariadb/server/sql/item_func.h` and
  `vendor/mariadb/server/sql/item_func.cc` define `SQL%ROWCOUNT`.
- `vendor/mariadb/server/sql/sql_lex.cc` defines
  `Lex_trim_st::make_item_func_trim_oracle()`.
- `vendor/mariadb/server/sql/item_strfunc.h`,
  `vendor/mariadb/server/sql/item_strfunc.cc`,
  `vendor/mariadb/server/sql/item_cmpfunc.h`, and
  `vendor/mariadb/server/sql/item_cmpfunc.cc` define the Oracle-specific
  string and `DECODE` item classes.

Current linked evidence after the `query-cache-size-profile` slice:

- The stripped linked open-close smoke binary is 8,390,256 bytes.
- `llvm-nm -C --size-sort` on the linked smoke binary still reports 211
  Oracle-named symbols.
- Large remaining Oracle-specific linked symbols include vtables for
  `Item_func_decode_oracle`, `Item_func_substr_oracle`,
  `Item_func_ltrim_oracle`, `Item_func_rtrim_oracle`,
  `Item_func_trim_oracle`, `Item_func_concat_operator_oracle`,
  `Item_func_regexp_replace_oracle`, `Item_func_replace_oracle`,
  `Item_func_lpad_oracle`, `Item_func_rpad_oracle`, and
  `Item_func_oracle_sql_rowcount`.

## Design

Add a MyLite-owned minsize build option,
`MYLITE_DISABLE_ORACLE_FUNCTIONS`, and enable it in
`tools/build-mariadb-minsize.sh`.

When enabled:

- omit Oracle-only native function aliases and builders from
  `item_create.cc`;
- build only the normal native function hash, not
  `native_functions_hash_oracle`;
- stop registering `oracle_schema` as a distinct built-in schema and bind
  `oracle_schema_ref` to the normal MariaDB schema only to satisfy inherited
  print helpers that compare schema addresses;
- compile MariaDB parser actions for Oracle-only constructs as explicit
  unsupported errors instead of instantiating Oracle-only item classes;
- omit `Item_func_oracle_sql_rowcount` and
  `Lex_trim_st::make_item_func_trim_oracle()` implementations.

Normal MariaDB function builders for `CONCAT`, `LPAD`, `LTRIM`, `REPLACE`,
`RPAD`, `RTRIM`, `SUBSTR`, and `TRIM` must remain registered and covered by
the open/close smoke.

## Non-goals

- Do not remove the normal MariaDB parser.
- Do not remove `MODE_ORACLE` from `sql_mode` parsing in this slice.
- Do not support Oracle-mode parsing; that was already made explicitly
  unsupported by `oracle-parser-size-profile`.
- Do not alter full non-embedded MariaDB server behavior unless the MyLite
  option is enabled.

## Compatibility impact

The aggressive MyLite minsize profile will no longer expose documented MariaDB
Oracle compatibility aliases such as `DECODE_ORACLE` and `LPAD_ORACLE`.
`oracle_schema`-qualified Oracle behavior is also unavailable in the minsize
profile.

This is a deliberate size experiment layered on top of the existing
Oracle-parser removal. Keeping it should depend on whether MyLite wants
Oracle-compatibility aliases in the default embedded profile.

## Single-file and embedded-lifecycle impact

This slice does not affect `.mylite` file format, catalog layout, storage
pages, locking, recovery, or open/close ownership. It only changes SQL
function registration and parser item construction in the embedded minsize
build.

## Binary-size impact

Expected linked savings are limited but real: the direct target is the
remaining Oracle-only function builders, vtables, item methods, schema object,
and hash table. The static archive may shrink less because many item classes
live in large shared SQL translation units.

Record final archive and stripped linked sizes after implementation.

## Test plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oracle-functions MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oracle-functions MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oracle-functions MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
git diff --check
```

Also inspect:

```sh
llvm-nm -C --size-sort build/mariadb-minsize-oracle-functions/mylite/mylite-open-close-smoke | rg 'Item_func_.*oracle|_ORACLE|_oracle|oracle_'
llvm-ar t build/mariadb-minsize-oracle-functions/libmysqld/libmariadbd.a | wc -l
```

## Acceptance criteria

- The minsize profile builds and links with `MYLITE_DISABLE_ORACLE_FUNCTIONS`.
- Current MyLite open/close and compatibility smokes pass.
- The open/close smoke proves normal MariaDB string functions still work.
- The open/close smoke proves an Oracle-only function alias fails explicitly.
- Oracle SQL-mode parsing remains explicitly unsupported.
- Size deltas are recorded in this spec and production-size analysis.

## Risks

- Some MariaDB parser actions still share grammar with the MariaDB parser even
  after the generated Oracle parser is omitted. Those actions must compile
  without Oracle item references when this option is enabled.
- Binding `oracle_schema_ref` to `mariadb_schema` is only acceptable in the
  minsize profile because no Oracle item classes should remain reachable.
- Removing the aliases is a compatibility tradeoff against documented MariaDB
  Oracle-migration behavior.
