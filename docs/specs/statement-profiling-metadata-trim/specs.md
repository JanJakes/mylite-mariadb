# Statement Profiling Metadata Trim

## Problem Statement

Statement profiling is already outside the MyLite embedded profile, but the
disabled embedded build still links the `INFORMATION_SCHEMA.PROFILING` field
metadata and old-format `SHOW PROFILE` builder from `sql_profile.cc`. That is
server diagnostic surface, not application SQL behavior. The embedded profile
should reject profiling metadata reads through the MyLite policy and link a
small fail-closed MariaDB stub by default.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_profile.cc` defines
  `Show::query_profile_statistics_info`,
  `fill_query_profile_statistics_info()`, and
  `make_profile_table_for_show()`. With `ENABLED_PROFILING=OFF`, the full
  measurement classes are already excluded, but this metadata object remains
  in `libmariadbd.a`.
- `mariadb/sql/sql_show.cc` registers `INFORMATION_SCHEMA.PROFILING` with the
  profiling field metadata, fill hook, and old-format builder.
- `mariadb/sql/sql_yacc.yy` routes `SHOW PROFILE` through
  `prepare_schema_table(..., SCH_PROFILES)`, so a replacement source must still
  provide the profiling symbols and fail closed if the MyLite policy is
  bypassed.
- `packages/libmylite/src/database.cc` already rejects top-level
  `SHOW PROFILE`, `SHOW PROFILES`, and profiling variable assignments.
  It did not reject direct `INFORMATION_SCHEMA.PROFILING` reads.

## Proposed Design

Add `MYLITE_WITH_STATEMENT_PROFILING_METADATA`, defaulting to `ON` for normal
MariaDB-compatible builds and forced `OFF` in the MyLite embedded baseline.
When disabled, build `mylite_sql_profile_disabled.cc` instead of
`sql_profile.cc`. The stub preserves the required symbols with a minimal
unsupported schema shape and returns `ER_NOT_SUPPORTED_YET` for profiling
fills or old-format `SHOW PROFILE` construction.
If a custom build enables MariaDB's `ENABLED_PROFILING`, CMake keeps the full
`sql_profile.cc` source because the profiling runtime classes are then required.

The MyLite SQL policy rejects qualified `INFORMATION_SCHEMA.PROFILING` reads
and unqualified `PROFILING` table references when the current schema is
`information_schema`, matching the optimizer-trace policy pattern. Application
tables named `profiling` remain usable in ordinary schemas.

## Compatibility Impact

No important MySQL/MariaDB application behavior is removed. Statement profiling
is a server diagnostic feature and already reports `@@have_profiling=NO` in
the embedded profile. The change makes the related Information Schema table
explicitly unsupported through the public MyLite API instead of leaving it as
an accidental disabled MariaDB surface.

## Database Directory And Native Storage Impact

None. This slice does not change durable file layout, native storage engines,
temporary files, transactions, locking, or database-directory lifecycle.

## Binary Size Impact

Measured with `tools/mariadb-embedded-build all`: the default embedded archive
is 26,511,064 bytes / 25.28 MiB, down 4,072 bytes from the previous embedded
profile. The stripped `mylite_sql_profile_disabled.cc.o` member is 2,656 bytes,
replacing the stripped 6,744-byte `sql_profile.cc.o` member.

## Test And Verification Plan

- Extend server-surface policy coverage for direct and prepared
  `INFORMATION_SCHEMA.PROFILING` reads.
- Cover unqualified `PROFILING` reads while `information_schema` is the current
  schema.
- Cover an ordinary application table named `profiling` to prevent policy
  overreach.
- Verify the embedded archive contains `mylite_sql_profile_disabled.cc.o`
  instead of `sql_profile.cc.o`.
- Run the standard embedded and first-party build, test, format, tidy, and
  whitespace checks.

## Acceptance Criteria

- The embedded baseline records
  `MYLITE_WITH_STATEMENT_PROFILING_METADATA=OFF`.
- Profiling metadata SQL fails through the MyLite server-surface policy.
- Application tables named `profiling` remain queryable.
- The embedded archive links the disabled profiling metadata source.
- Documentation describes profiling metadata as omitted from the default
  embedded archive.
