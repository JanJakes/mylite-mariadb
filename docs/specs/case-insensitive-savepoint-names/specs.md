# Case-Insensitive Savepoint Names

## Goal

Align MyLite-owned savepoint lookup with MariaDB's savepoint identifier
comparison. Direct and prepared `SAVEPOINT`, `ROLLBACK TO [SAVEPOINT]`, and
`RELEASE SAVEPOINT` should find matching savepoints case-insensitively across
simple, backtick-quoted, and ANSI_QUOTES double-quoted names.

## Non-Goals

- Handler-level MariaDB savepoint hooks.
- Changing the MyLite checkpoint stack model.
- SQL identifier comparison changes outside savepoint names.
- Transactional DDL, MEMORY/HEAP row savepoints, XA, isolation, or fully
  transactional handler flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:18319-18338` parses savepoint names as `ident`.
- `mariadb/sql/transaction.cc:586-599` finds savepoints through
  `Lex_ident_savepoint::streq()`.
- `mariadb/sql/transaction.cc:609-626` uses that same lookup when replacing a
  duplicate savepoint name.
- `mariadb/sql/lex_ident.h:42-47` defines case-insensitive identifier
  comparison through `my_charset_utf8mb3_general1400_as_ci`.
- `mariadb/sql/lex_ident.h:108-129` implements `Lex_ident::streq()` through
  the selected charset's `streq()`.
- `mariadb/sql/lex_ident.h:373-376` defines `Lex_ident_savepoint` as a
  `Lex_ident_ci`.
- `mariadb/include/m_ctype.h:1111-1122` exposes `CHARSET_INFO::strnncoll()`
  for non-null-terminated byte spans.

## Compatibility Impact

This closes a behavioral mismatch in the bounded file-backed row-DML
transaction path: savepoint replacement, rollback, and release now use
MariaDB's case-insensitive savepoint-name comparison. Compatibility remains
partial because the operations still execute through MyLite-owned storage
checkpoints rather than MariaDB handler-level savepoint hooks.

## Design

Add a MyLite helper that compares two decoded savepoint-name strings using
`my_charset_utf8mb3_general1400_as_ci.strnncoll() == 0`, matching MariaDB's
`Lex_ident_savepoint` collation. Use the helper in:

- duplicate savepoint replacement after `SAVEPOINT name`,
- `ROLLBACK TO [SAVEPOINT] name` target lookup,
- `RELEASE SAVEPOINT name` target lookup.

Parsing remains unchanged. The helper compares the decoded identifier string
produced by the existing simple, backtick-quoted, and ANSI_QUOTES
double-quoted savepoint parser.

## File Lifecycle

No file-format or companion-file behavior changes. Case-insensitive lookup
only changes which existing nested storage checkpoint frame is found.

## Embedded Lifecycle And API

No public C API changes are required. The behavior is visible through existing
direct SQL execution and prepared-statement APIs.

## Build, Size, And Dependencies

No new dependency is added. `libmylite` already links against the MariaDB
embedded runtime; this slice uses MariaDB's exported charset object for the
comparison to avoid a local approximation.

## Test Plan

- Add direct transaction policy coverage for mixed-case rollback and release.
- Add storage-engine transaction coverage proving case-insensitive duplicate
  savepoint replacement preserves changes made before the replacement and rolls
  back changes made after it.
- Add prepared storage-engine coverage proving savepoint, rollback, and release
  prepared under different case variants find the same target.
- Run dev, embedded, storage-smoke, compatibility harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct savepoint rollback and release find targets case-insensitively.
- Duplicate savepoint replacement is case-insensitive.
- Prepared savepoint-control statements find targets case-insensitively.
- Docs no longer describe savepoint-name comparison as byte-for-byte.

## Risks And Open Questions

- This still does not solve handler-level savepoint hooks. The storage
  checkpoint stack remains the execution mechanism until MyLite can model
  MariaDB handler savepoint semantics directly.
