# Oracle Compatibility Function Trim

## Problem

The embedded profile already rejects `sql_mode=ORACLE` and links an
unsupported Oracle parser stub, but the archive still carries Oracle-only
function aliases, `oracle_schema` routing, a separate Oracle native-function
hash, and item classes used only by Oracle compatibility behavior. Those
surfaces are optional MariaDB Oracle-migration behavior, not core
MySQL/MariaDB application SQL.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/item_create.cc` registers Oracle aliases such as
  `CONCAT_OPERATOR_ORACLE`, `DECODE_ORACLE`, `LPAD_ORACLE`, `LTRIM_ORACLE`,
  `REPLACE_ORACLE`, `RPAD_ORACLE`, `RTRIM_ORACLE`, and `SUBSTR_ORACLE`.
- `mariadb/sql/item_create.cc` also builds `native_functions_hash_oracle` by
  adding normal native functions and replacing selected names with Oracle
  variants.
- `mariadb/sql/sql_schema.cc` defines `Schema_oracle`, registers
  `oracle_schema`, and routes implied lookup to Oracle behavior when
  `MODE_ORACLE` is set.
- `mariadb/sql/sql_yacc.yy` can still instantiate Oracle-only items for
  `||`, `SQL%ROWCOUNT`, and `TRIM_ORACLE`.
- `mariadb/sql/item_func.*`, `mariadb/sql/item_cmpfunc.*`,
  `mariadb/sql/item_strfunc.*`, `mariadb/sql/sql_lex.cc`, and
  `mariadb/sql/structs.h` define the Oracle-only item classes and trim helper.

## Design

Add `MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS`, defaulting to `ON`, and set it to
`OFF` in `cmake/mariadb-embedded-baseline.cmake`.

When the option is off, the embedded archive:

- omits Oracle-only native function aliases and builders;
- omits `native_functions_hash_oracle` and the Oracle override registry;
- omits `Schema_oracle` and `oracle_schema` lookup;
- keeps parser actions for Oracle-only constructs fail-closed with an explicit
  unsupported diagnostic;
- omits Oracle-only item classes and helper methods.

Normal `CONCAT`, `LPAD`, `RPAD`, `LTRIM`, `RTRIM`, `SUBSTR`, `REPLACE`,
`TRIM`, and `LENGTH` remain registered and covered.

## Non-Goals

- Do not remove the generated MariaDB parser.
- Do not remove SQL-mode parsing broadly.
- Do not remove JSON, GEOMETRY/GIS, collations, or other important
  application-facing SQL features.
- Do not change non-embedded MariaDB behavior unless the MyLite option is
  explicitly disabled.

## Compatibility Impact

The default embedded profile no longer exposes Oracle compatibility aliases
such as `DECODE_ORACLE` and `LPAD_ORACLE`, and `oracle_schema`-qualified
Oracle behavior is unavailable. This is consistent with Oracle SQL mode being
out of scope. Ordinary MySQL/MariaDB string functions remain available.

## Directory And Lifecycle Impact

This slice only changes function registration and Oracle-only expression item
construction. It does not affect database-directory layout, native storage
files, transaction behavior, recovery, locking, or public handle ownership.

## Binary-Size Impact

Measured on 2026-05-21 with:

```sh
tools/mariadb-embedded-build configure
rm -f build/mariadb-embedded/libmysqld/libmariadbd.a \
  build/mariadb-embedded/libmysqld/libmariadbd.a.mylite-stripped
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
```

The previous stripped archive was 27,005,960 bytes. After this slice the
stripped archive is 26,861,088 bytes, 25.62 MiB, with 705 members. The
incremental stripped-archive saving is 144,872 bytes.

## Test Plan

Run:

```sh
tools/mariadb-embedded-build configure
rm -f build/mariadb-embedded/libmysqld/libmariadbd.a \
  build/mariadb-embedded/libmysqld/libmariadbd.a.mylite-stripped
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
cmake --build --preset embedded-dev
ctest --preset embedded-dev -L compat.server-surface --output-on-failure
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
tools/mariadb-embedded-build all
```

## Acceptance Criteria

- The embedded baseline reports `MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS=OFF`.
- Oracle-only aliases fail predictably in direct and prepared execution.
- `oracle_schema` routing is unavailable as a built-in compatibility schema in
  the default embedded profile.
- Normal MySQL/MariaDB string functions covered by the same item families still
  execute successfully.
- The archive size delta is recorded in the size-profile docs.

## Risks

- Some Oracle-only grammar remains in the MariaDB parser. Parser actions must
  fail closed instead of retaining references to omitted classes.
- `oracle_schema_ref` exists for inherited print helper signatures. With
  Oracle item classes omitted, normal item printing must not accidentally print
  Oracle downgrade names.
