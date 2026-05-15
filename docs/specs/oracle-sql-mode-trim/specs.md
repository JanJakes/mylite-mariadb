# Oracle SQL Mode Trim

## Problem

MyLite targets MySQL/MariaDB application compatibility in an embedded
single-file runtime. MariaDB's `SQL_MODE=ORACLE` is a separate Oracle Database
compatibility mode with its own generated parser and PL/SQL-oriented behavior.
Keeping the Oracle parser object in the default embedded archive preserves a
large compatibility surface that MyLite does not plan to support in the core
library.

The bundle-size research records a large successful trim for removing the
generated Oracle parser object. This slice turns that evidence into an explicit
MyLite profile option and a stable public SQL policy for attempts to enable
Oracle SQL mode.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/CMakeLists.txt:72-84` generates both `yy_mariadb.yy` and
  `yy_oracle.yy` from `sql_yacc.yy`.
- `mariadb/sql/CMakeLists.txt:116-118` adds both generated parser objects to
  the normal `sql` target.
- `mariadb/libmysqld/CMakeLists.txt:31-47` adds both generated parser objects
  to the embedded `sql_embedded` archive.
- `mariadb/sql/CMakeLists.txt:429-431` builds the Oracle parser through Bison
  with the `ORA` prefix.
- `mariadb/sql/sql_parse.cc:10306` declares `ORAparse()`, and
  `mariadb/sql/sql_parse.cc:10365-10366` dispatches to it when
  `thd->variables.sql_mode & MODE_ORACLE` is set.
- `mariadb/sql/sys_vars.cc:4019` includes `ORACLE` in the SQL mode names
  accepted by the `sql_mode` system variable.
- MariaDB documentation describes `SQL_MODE` as a mechanism for emulating
  other SQL servers. It specifically describes `ORACLE` mode as enabling
  Oracle Database SQL syntax and behavior, including PL/SQL-oriented syntax,
  and documents `SET SQL_MODE='ORACLE'`.

Official MariaDB references:

- <https://mariadb.com/docs/server/server-management/variables-and-modes/sql_mode>
- <https://mariadb.com/docs/release-notes/community-server/about/compatibility-and-differences/sql_modeoracle>

## Scope

- Add a `MYLITE_WITH_ORACLE_SQL_MODE` CMake option that defaults to `ON`.
- Set `MYLITE_WITH_ORACLE_SQL_MODE=OFF` in the MyLite embedded profile.
- When the option is off, replace the generated `yy_oracle.cc` object in both
  the normal `sql` target and embedded `sql_embedded` archive with a small
  MyLite-owned `ORAparse()` stub.
- Keep generation of `yy_oracle.yy` / `yy_oracle.hh` for now so the Bison and
  lexer build graph remains close to upstream.
- Reject public direct and prepared `SET sql_mode` statements that include the
  `ORACLE` mode token before MariaDB execution.
- Keep ordinary MySQL/MariaDB SQL modes available.

## Non-Goals

- Remove Oracle-mode token definitions, lexer branches, SQL mode enum bits, or
  Oracle compatibility helper functions in this slice.
- Remove MariaDB functions that exist in all modes and are useful to
  MySQL/MariaDB applications merely because they also support Oracle mode.
- Remove stored-program runtime or package support beyond the existing MyLite
  non-table object policy.
- Implement an Oracle compatibility adapter.

## Design

Define `MYLITE_WITH_ORACLE_SQL_MODE` in both SQL build roots:

- `mariadb/sql/CMakeLists.txt` for the normal `sql` target;
- `mariadb/libmysqld/CMakeLists.txt` for the embedded `sql_embedded` archive.

When enabled, the build keeps MariaDB's generated `yy_oracle.cc` object. When
disabled, it links `mariadb/sql/mylite_oracle_parser_disabled.cc` instead. The
stub keeps the generated parser's public symbols that retained MariaDB code
references:

- `int ORAparse(THD *thd)`;
- `void turn_parser_debug_on_ORAparse()` in debug builds.

Supported MyLite entry points should not reach the stub because `libmylite`
rejects `SET sql_mode` attempts that include `ORACLE`. The stub still fails
closed with a MariaDB unsupported diagnostic if an internal bypass sets
`MODE_ORACLE` and then parses another statement.

The public SQL policy will inspect `SET` statements for `sql_mode` assignments
whose token or quoted value list contains `ORACLE`, including representative
forms such as:

- `SET sql_mode=ORACLE`;
- `SET sql_mode='ORACLE'`;
- `SET SESSION sql_mode='ANSI,ORACLE'`;
- `SET @@sql_mode='ORACLE'`.

The policy is assignment-scoped so user variables such as `@sql_mode` and
unrelated assignments containing the word `ORACLE` do not trigger the
system-variable policy.

## Compatibility Impact

MyLite loses an Oracle Database migration surface, not core MySQL/MariaDB
compatibility. Ordinary MariaDB SQL modes such as strictness, ANSI quotes,
`NO_ENGINE_SUBSTITUTION`, and similar MySQL/MariaDB-oriented modes remain
available unless a later slice evaluates them separately.

Direct and prepared public entry points return a stable MyLite error before
MariaDB execution when a statement tries to enable Oracle mode.

## Single-File And Embedded-Lifecycle Impact

No durable file, sidecar, startup, shutdown, or recovery behavior changes. The
trim removes an unreachable parser object from the default embedded profile.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

None. The slice affects SQL parsing mode selection before storage-engine
routing.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. A future compatibility adapter that intentionally
targets Oracle-mode syntax would need its own policy and build profile.

## Binary-Size Impact

`docs/architecture/bundle-size-research.md` records prior evidence for removing
the generated Oracle parser object at 589,824 bytes from a stripped linked
runtime proxy and 1,412,822 bytes from the embedded archive.

Measured on the current branch after implementation:

- default embedded archive:
  `30,958,576 bytes / 29.52 MiB`, down 968,128 bytes from the prior
  server-utility-trim baseline;
- storage-smoke archive:
  `31,139,160 bytes / 29.70 MiB`, down 968,128 bytes from the prior
  server-utility-trim baseline;
- representative stripped linked smoke binaries dropped by roughly 0.60 MiB,
  with `mylite_embedded_open_close_test` at
  `16,955,104 bytes / 16.17 MiB` and the storage-engine smoke at
  `17,286,368 bytes / 16.49 MiB`.

## License Or Dependency Impact

No new dependencies and no license change.

## Test And Verification Plan

- Add direct execution policy tests for representative `SET sql_mode` /
  `SET @@sql_mode` forms that include `ORACLE`.
- Add direct execution regression coverage for `@sql_mode` user variables and
  unrelated `ORACLE` user-variable assignments in the same `SET` statement.
- Add prepared-statement policy coverage for representative Oracle-mode
  assignments.
- Verify ordinary non-Oracle SQL mode assignments still work.
- Reconfigure and rebuild the default embedded MariaDB archive and the
  storage-smoke MariaDB archive.
- Build and run embedded and storage-smoke presets.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, normal MariaDB `sql` target, and
  first-party tidy checks.

## Acceptance Criteria

- The default MyLite embedded profile records `MYLITE_WITH_ORACLE_SQL_MODE=OFF`.
- Embedded archives link the MyLite Oracle parser stub instead of
  `yy_oracle.cc.o`.
- Public direct and prepared entry points reject attempts to enable Oracle SQL
  mode with stable MyLite diagnostics.
- Ordinary non-Oracle `sql_mode` assignments continue to work.
- Size documentation records the current measured impact.

## Risks And Open Questions

- This slice keeps Oracle-mode lexer branches and helper functions, so the
  measured delta may be smaller than the historical branch's later, broader
  Oracle compatibility trims.
- Some applications may set broad portability modes that include `ORACLE`.
  Rejecting those assignments is intentional because they route future parsing
  through an unsupported Oracle compatibility parser.
