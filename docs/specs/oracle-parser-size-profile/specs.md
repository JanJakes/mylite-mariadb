# Oracle parser size profile

## Problem

The current MyLite minsize profile still compiles and links MariaDB's generated
Oracle SQL-mode parser. The parser is a large single archive member, and MyLite
has not claimed Oracle SQL mode as part of its embedded compatibility surface.

This slice tests whether the aggressive minsize profile can omit the Oracle
parser while keeping normal MariaDB SQL parsing intact.

## Source findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `vendor/mariadb/server/sql/CMakeLists.txt` generates both
  `yy_mariadb.yy` and `yy_oracle.yy` from `sql_yacc.yy` through
  `gen_yy_files.cmake`, then runs separate Bison targets for
  `yy_mariadb.cc` and `yy_oracle.cc`.
- The same CMake file includes `${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.cc` in
  `SQL_SOURCE` for the server SQL library.
- `vendor/mariadb/server/libmysqld/CMakeLists.txt` includes
  `${CMAKE_BINARY_DIR}/sql/yy_oracle.cc` in `GEN_SOURCES`, and
  `SQL_EMBEDDED_SOURCES` includes `GEN_SOURCES`.
- `vendor/mariadb/server/sql/sql_parse.cc:parse_sql()` dispatches to
  `ORAparse(thd)` when `thd->variables.sql_mode & MODE_ORACLE`, otherwise to
  `MYSQLparse(thd)`.
- `vendor/mariadb/server/sql/sql_class.cc:THD::sql_parser()` uses the same
  dispatch for parser fragments.
- `vendor/mariadb/server/sql/sql_priv.h` declares
  `turn_parser_debug_on_ORAparse()` for debug parser tracing.

Current profile measurement:

- `yy_oracle.cc.o` in `libmariadbd.a`: 1,372,688 bytes,
- `yy_mariadb.cc.o` in `libmariadbd.a`: 1,386,632 bytes.

## Design

Add a MyLite-owned minsize build option that omits the generated Oracle parser
from the embedded archive and links a tiny Oracle-parser stub instead.

When `sql_mode=ORACLE` reaches the parser in this profile, the stub should
report `ER_NOT_SUPPORTED_YET` for Oracle SQL mode and return a parse failure.
This keeps the failure explicit and avoids link-time references to the omitted
`yy_oracle.cc`.

The default MariaDB parser must still be generated and compiled from the same
upstream grammar.

## Non-goals

- Do not remove `MODE_ORACLE` from `sql_mode` parsing in this slice.
- Do not remove Oracle-specific SQL functions or type behavior that are still
  compiled into shared SQL objects.
- Do not change the default SQL mode.
- Do not touch the full non-embedded MariaDB server build unless required by
  shared CMake structure.

## Compatibility impact

The aggressive MyLite minsize profile will no longer support executing
statements after switching the session to Oracle SQL mode. MariaDB SQL mode and
the current MyLite compatibility harness must continue to pass.

Because this slice does not remove all Oracle-mode helper code, it is a parser
size reduction rather than a complete Oracle compatibility purge.

## Single-file and embedded-lifecycle impact

This slice should not affect `.mylite` file format, catalog layout, storage
pages, locking, recovery, or open/close ownership. The affected lifecycle point
is embedded query parsing.

## Binary-size impact

The direct raw archive opportunity is the 1,372,688-byte `yy_oracle.cc.o`
member. Final linked savings may be smaller because the parser shares symbols,
tables, and relocation shape with the rest of the SQL layer.

After implementation, the measured size is:

- `libmariadbd.a`: 35,944,110 bytes,
- archive objects: 494,
- `mylite-open-close-smoke`: 18,576,832 bytes,
- stripped `mylite-open-close-smoke`: 15,850,736 bytes,
- `size` total: 16,111,856 bytes.

Compared with the charset-small profile, this saves 1,412,822 bytes from
`libmariadbd.a` and 589,824 bytes from the stripped linked open-close proxy.
Compared with the original production-size baseline, the combined
type-plugin, charset-small, and Oracle-parser profile saves 7,461,322 bytes
from `libmariadbd.a` and 3,481,168 bytes from the stripped linked open-close
proxy.

## Test plan

Run:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Also inspect:

```sh
ar t build/mariadb-minsize/libmysqld/libmariadbd.a | grep yy_oracle
```

The current profile should no longer include `yy_oracle.cc.o`. If a stub object
is added, it should be small and explicitly named.

## Verification

Run on 2026-05-12:

```sh
MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

All passed. `libmariadbd.a` contains `yy_mariadb.cc.o` and
`mylite_oracle_parser_stub.cc.o`, and no longer contains `yy_oracle.cc.o`.
The open/close smoke verifies that `SET sql_mode=ORACLE` followed by parsing a
statement fails with `ER_NOT_SUPPORTED_YET` and the explicit MyLite minsize
profile message.

## Acceptance criteria

- The minsize profile builds and links without `yy_oracle.cc.o` in
  `libmariadbd.a`.
- Current MyLite open/close and compatibility smokes pass.
- Oracle SQL-mode parsing fails explicitly instead of crashing or producing an
  unresolved symbol.
- Size deltas are recorded in this spec and in production-size analysis.

## Risks

- Some generated headers or lexer helpers may assume `yy_oracle.hh` exists even
  when `yy_oracle.cc` is omitted.
- `sql_mode=ORACLE` may appear in inherited system-table metadata even if
  MyLite does not execute Oracle-mode statements.
- This is an aggressive compatibility tradeoff; retaining the attempt should
  depend on whether MyLite wants Oracle SQL mode in its default embedded
  profile.
