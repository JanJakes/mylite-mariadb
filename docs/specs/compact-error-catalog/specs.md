# Compact Error Catalog

## Problem

The embedded profile links MariaDB's full English server error-message catalog.
That catalog provides detailed text for every server error even though MyLite's
public compatibility contract is based on MariaDB error numbers, SQLSTATEs, and
common diagnostics. Keeping every uncommon server diagnostic string increases
the embedded archive without adding SQL, storage, API, or directory-lifecycle
functionality.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/derror.cc` initializes server error messages in
  `init_errmessage()`.
- For English messages, `mariadb/sql/derror.cc` includes generated
  `mysqld_ername.h`, whose rows contain the error symbol, numeric error id, and
  full format string.
- `mariadb/sql/unireg.h` defines `ER_DEFAULT()` and `ER_THD()` lookups through
  the server error-message tables.
- `mariadb/sql/derror.cc` registers populated error ranges with
  `my_error_register()` and assigns selected `mysys` file-error messages in
  `init_myfunc_errs()`.
- `mariadb/libmysqld/libmysql.c`, `mariadb/libmysqld/lib_sql.cc`, and the
  `libmylite` wrapper copy MariaDB error numbers, SQLSTATEs, and message text
  into public diagnostics.

## Design

Add `MYLITE_WITH_FULL_ERROR_MESSAGES`, defaulting to `ON`, and set it to `OFF`
in the embedded baseline.

When the option is off, English server-message initialization uses a compact
catalog instead of including `mysqld_ername.h`:

- register the same active error-id ranges so MariaDB error numbers and
  SQLSTATE behavior remain intact;
- fill all registered error slots with a placeholder-free generic
  `"MariaDB error"` text;
- preserve a focused set of common MyLite-facing diagnostics such as syntax
  errors, duplicate keys, table existence and lookup errors, storage-engine
  file errors, unsupported-feature diagnostics, and unknown function paths;
- keep non-English file-based loading behavior unchanged when a non-English
  `lc_messages` profile is explicitly selected.

The compact catalog is an embedded-profile size choice. Normal MariaDB builds
keep the full upstream catalog unless the MyLite option is explicitly disabled.

## Non-Goals

- Do not change MariaDB error numbers or SQLSTATEs.
- Do not remove SQL syntax, SQL functions, JSON, GEOMETRY/GIS, collations,
  native storage engines, prepared statements, transactions, or DDL behavior.
- Do not change first-party MyLite diagnostics such as misuse, filesystem, or
  policy-rejection messages.
- Do not remove client-library error messages from `sql-common/errmsg.c`.

## Compatibility Impact

Applications that use MariaDB error numbers or SQLSTATEs keep the same behavior.
Common MyLite-facing messages remain readable and covered. Less common inherited
server errors may return a generic `"MariaDB error"` message in the
default embedded profile. That is a diagnostic-text compatibility tradeoff, not
a SQL or storage behavior change.

## Directory And Lifecycle Impact

None. This slice only changes compiled English error-message text. It does not
create, move, or remove database-directory files and does not affect open/close,
locking, recovery, or temporary-file placement.

## Binary-Size Impact

Measured on 2026-05-21 with:

```sh
tools/mariadb-embedded-build configure
rm -f build/mariadb-embedded/libmysqld/libmariadbd.a \
  build/mariadb-embedded/libmysqld/libmariadbd.a.mylite-stripped
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
```

The previous stripped archive was 26,861,088 bytes. After this slice the
stripped archive is 26,647,312 bytes, 25.41 MiB, with 705 members. The
incremental stripped-archive saving is 213,776 bytes.

## Test Plan

Run:

```sh
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
```

## Acceptance Criteria

- The embedded baseline reports `MYLITE_WITH_FULL_ERROR_MESSAGES=OFF`.
- MariaDB error numbers and SQLSTATEs remain covered by existing tests.
- Syntax-error and duplicate-key diagnostics remain readable.
- At least one uncommon inherited server error returns the compact generic
  diagnostic text while the MariaDB error number remains available separately.
- The archive size delta is recorded in the build and size-profile docs.

## Risks

- Overly aggressive message compaction could make common application diagnostics
  less useful. The retained-message list should be conservative for errors that
  existing tests or ordinary embedded workflows expose.
- Format strings must remain valid for callers that pass arguments to
  `my_error()`. The generic fallback intentionally has no placeholders, and
  preserved messages keep their upstream placeholder signatures.
