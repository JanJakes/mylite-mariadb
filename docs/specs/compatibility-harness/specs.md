# Compatibility Harness

## Goal

Make compatibility coverage runnable in stable, named groups so storage,
recovery, directory-boundary, MariaDB-reference, and application-query work can
grow without turning every slice into a bespoke test command.

## Non-Goals

- Do not require a daemon or external MariaDB server in the default harness.
- Do not add prepared statements; that is the next roadmap slice.
- Do not broaden engine support beyond the native MyISAM and explicit InnoDB
  surfaces already covered.
- Do not perform size profile hardening.

## Design

The harness uses CTest labels as the public grouping contract. The embedded
preset exposes these repeatable groups:

| Group | Command | Purpose |
| --- | --- | --- |
| Lifecycle | `ctest --preset embedded-dev -L compat.lifecycle` | Embedded open, close, basic execution, and reopen coverage. |
| Directory boundary | `ctest --preset embedded-dev -L compat.directory-boundary` | Tests that assert durable and transient files stay under the MyLite database directory. |
| MariaDB comparison | `ctest --preset embedded-dev -L compat.mariadb-comparison` | SQL cases with result vectors pinned to MariaDB 11.8 behavior. |
| Crash/reopen | `ctest --preset embedded-dev -L compat.crash-reopen` | Child-process exit, stale runtime state, and recovery behavior. |
| Application queries | `ctest --preset embedded-dev -L compat.application-query` | Representative application-style joins, aggregates, pagination, updates, and reopen persistence. |
| Query surface | `ctest --preset embedded-dev -L compat.query` | All SQL execution coverage that currently belongs to compatibility tracking. |

The first MariaDB comparison group is a fixed expected-result suite rather than
a live server comparison. That keeps normal development serverless while giving
future slices a stable place to add either more reference vectors or an optional
external-server runner.

## Test Plan

1. Add labels to existing embedded tests without changing their behavior.
2. Add `libmylite.compat.mariadb-comparison` for reference SQL expression
   results, including string, arithmetic, date, and `NULL` semantics.
3. Add `libmylite.compat.application-queries` for an application-shaped InnoDB
   schema with joins, aggregates, pagination, update counts, close, and reopen.
4. Run each compatibility label group directly.
5. Run the full `dev` and `embedded-dev` test, format, tidy, and size checks.

## Acceptance Criteria

- Each documented compatibility group is runnable with `ctest -L`.
- The harness includes non-empty MariaDB-reference and application-query groups.
- Directory-boundary coverage remains part of the grouped harness.
- Crash/reopen coverage remains part of the grouped harness.
- Documentation and the roadmap describe the new harness contract.
