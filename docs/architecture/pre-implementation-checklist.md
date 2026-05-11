# Pre-implementation checklist

This checklist captures decisions that should be made before MyLite imports
or changes MariaDB source in earnest.

## Upstream base

- Choose an exact MariaDB 11.8 LTS tag or commit as the initial base.
- Record the upstream repository URL, tag, commit, and release status.
- The current preferred initial import ref is `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), subject to a fresh release
  check immediately before import.
- Keep an upstream-tracking branch separate from MyLite patch branches.
- Decide whether early work tracks the `11.8` branch or a specific patch tag.
  The branch is useful for observation; implementation should pin a ref.

## Build environment

- Create a reproducible build environment, preferably a Linux container first
  and macOS as a secondary supported development platform.
- Pin required tools such as CMake, Bison, compiler family, OpenSSL, and PCRE2.
- Produce a baseline embedded build of `libmariadbd.a`.
- Record the size of the embedded artifact and the list of linked static
  engines/plugins.
- Add a repeatable command for a minimal embedded profile before optimizing for
  size.

## Source layout and patch stack

- Decide how upstream MariaDB source enters this repository.
- Keep mechanical upstream imports separate from MyLite commits.
- Keep MyLite-owned code in clearly named modules where possible.
- Preserve upstream formatting in MariaDB-derived files.
- Document every broad fork delta in the slice spec or commit body.

## Legal, license, and trademark

- Treat MyLite as GPL-2.0-only while it contains MariaDB-derived server code.
- Document that SQLite-like lifecycle semantics do not imply permissive
  SQLite-like licensing.
- Review whether the MyLite name and public packaging are acceptable under
  MySQL and MariaDB trademark rules.
- Add clear no-affiliation notices before distributing public binaries or
  packages.
- Preserve upstream copyright and license notices.

## First engineering slices

Use [../ROADMAP.md](../ROADMAP.md) as the ordered progress tracker. Keep this
checklist focused on gates that should be satisfied before and during the first
implementation slices.

Write slice specs before implementation for:

- `upstream-11-8-import`
- `build-profile-minsize`
- `embedded-bootstrap`
- `libmylite-open-close`
- `storage-engine-skeleton`
- `mylite-engine-discovery`
- `ddl-metadata-routing`
- `single-file-catalog`
- `unsupported-server-surface`

Each slice should state scope, source refs, design, tests, acceptance criteria,
and known risks.

## Test harness

- Identify MariaDB tests that can run against embedded mode early.
- Add file-system checks that distinguish expected MyLite companion files from
  unexpected MariaDB datadir, schema, engine, or log sidecars.
- Add repeated open/close lifecycle tests.
- Add basic crash/reopen tests once file writes exist.
- Add a MariaDB reference runtime for compatibility-sensitive SQL behavior.
- Keep unit, embedded smoke, MariaDB-comparison, and crash tests separately
  labeled.

## File format and storage decisions

- Decide whether v1 uses a custom pager, SQLite pager/B-tree code, or another
  GPL-compatible storage component.
- Define initial file header fields, magic bytes, page size, checksum policy,
  and format-version policy.
- Decide how table definition images, SQL text, and `tabledef_version` are
  stored.
- Decide whether rollback journal or WAL state lives inside the `.mylite` file
  or in documented MyLite-owned companion files.
- Define the minimum crash-safety guarantee before storing user data.
- Define how schemas, system tables, grants policy, and autoincrement state are
  represented in the catalog.

## Product scope

- Define v1 write concurrency honestly and avoid permanently closing the path to
  safer in-process write concurrency later.
- Decide which journal, WAL, lock, shared-memory, and temporary files are
  allowed and how strict no-temp-file or no-companion-files modes behave.
- Decide whether named time zones, events, stored routines, triggers, views,
  foreign keys, fulltext, spatial indexes, and generated columns are in v1 or
  explicitly deferred.
- Decide how unsupported server features fail: parse error, stable MyLite
  error, MariaDB diagnostic, warning, or documented no-op.

## Binary size and dependencies

- Establish an initial size baseline before making size promises.
- Define the default embedded profile by explicitly listing omitted
  running-server-specific and rare optional components.
- Track size changes per implementation slice once code exists.
- Require a license and size rationale for every new dependency.
- Keep dynamic plugin loading out of the default embedded profile.
