# Regex function size profile

## Problem

The aggressive MyLite minsize profile still links PCRE2 through the embedded
MariaDB archive. MyLite does not need regular expression support for its
current embedded lifecycle, storage-engine, or single-file goals, and a package
that vendors runtime libraries pays for `libpcre2-8.so` even when the host
application only uses ordinary SQL.

This slice removes MariaDB regular expression SQL surfaces from the aggressive
minsize profile and verifies whether that also removes the PCRE2 runtime
dependency.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `vendor/mariadb/server/libmysqld/CMakeLists.txt` always merges `pcre2-8`
  into `mysqlserver` through the embedded `LIBS` list.
- `vendor/mariadb/server/sql/CMakeLists.txt` links `pcre2-8` into the daemon
  SQL target; the embedded target has its own library list.
- `vendor/mariadb/server/sql/item_cmpfunc.h` defines
  `Regexp_processor_pcre`, `Item_func_regex`, and `Item_func_regexp_instr`.
- `vendor/mariadb/server/sql/item_cmpfunc.cc` includes `pcre2.h` and implements
  the `REGEXP` operator and `REGEXP_INSTR()` processor.
- `vendor/mariadb/server/sql/item_strfunc.h` and
  `vendor/mariadb/server/sql/item_strfunc.cc` define and implement
  `REGEXP_REPLACE()` and `REGEXP_SUBSTR()`.
- `vendor/mariadb/server/sql/item_create.cc` registers native builders for
  `REGEXP_INSTR`, `REGEXP_REPLACE`, and `REGEXP_SUBSTR`.
- `vendor/mariadb/server/sql/sql_yacc.yy` parses the `REGEXP` and `RLIKE`
  operators directly into `Item_func_regex`.
- `vendor/mariadb/server/sql/sys_vars.cc` exposes `default_regex_flags` and
  uses PCRE2 constants for that system variable.

## Design

Add `MYLITE_DISABLE_REGEX_FUNCTIONS` for the embedded build.

When enabled:

- keep the `REGEXP`/`RLIKE` grammar tokens parseable, but make
  `Item_func_regex::fix_length_and_dec()` fail with a stable unsupported
  diagnostic;
- remove native builders and item classes for `REGEXP_INSTR()`,
  `REGEXP_REPLACE()`, and `REGEXP_SUBSTR()`, so those names fail through
  MariaDB's ordinary unknown-function path;
- exclude real PCRE-backed `Regexp_processor_pcre` code from the embedded
  object files;
- remove `pcre2-8` from the embedded `mysqlserver` merge list when the profile
  is enabled;
- leave the daemon `sql` target unchanged.

This is an aggressive size profile, not a default compatibility claim.

## Non-Goals

- Do not replace PCRE2 with a smaller regular expression engine.
- Do not change ordinary `LIKE` semantics.
- Do not remove parser keywords or regenerate broad parser output unless the
  build requires it.
- Do not remove `default_regex_flags` in this slice; it is low value once the
  execution surfaces are disabled.

## Compatibility Impact

The minsize profile no longer supports:

- `expr REGEXP expr`
- `expr RLIKE expr`
- `REGEXP_INSTR()`
- `REGEXP_REPLACE()`
- `REGEXP_SUBSTR()`

`LIKE` remains supported. This is a visible MariaDB SQL compatibility loss and
belongs only in the most aggressive size profile unless product scope later
accepts it.

## Single-File and Embedded-Lifecycle Impact

No file-format, storage-engine, catalog, or sidecar behavior changes are
expected. This is a SQL expression and dependency-footprint slice.

## Binary-Size Impact

Expected effects:

- remove `libpcre2-8.so` from linked smoke dependencies;
- reduce linked executable size slightly by omitting PCRE call sites and
  regular expression item implementations;
- reduce the embedded archive modestly because `item_cmpfunc.cc`,
  `item_strfunc.cc`, and `item_create.cc` compile less code.

## Test Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-regex-functions MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-regex-functions MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-regex-functions MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Also inspect:

```sh
ldd build/mariadb-minsize-regex-functions/mylite/mylite-open-close-smoke
nm -S build/mariadb-minsize-regex-functions/mylite/mylite-open-close-smoke
```

The linked smoke should not depend on `libpcre2-8.so` and should not contain
PCRE2 symbols.

## Acceptance Criteria

- The minsize build succeeds with `MYLITE_DISABLE_REGEX_FUNCTIONS=ON`.
- Current MyLite open/close and compatibility harness smokes pass.
- `REGEXP`/`RLIKE` fail with a stable unsupported diagnostic.
- `REGEXP_INSTR()`, `REGEXP_REPLACE()`, and `REGEXP_SUBSTR()` fail as unknown
  functions.
- `LIKE` still works.
- `mylite-open-close-smoke` has no dynamic `libpcre2-8` dependency.
- Production size analysis records measured archive, linked, stripped-linked,
  and bundled-dependency deltas.

## Risks

- `REGEXP` is parsed as an operator, not only as a native function, so it needs
  a retained unsupported item path instead of only removing function builders.
- `default_regex_flags` remains visible even though execution surfaces are
  unavailable, which is slightly odd but smaller than a broader sysvar removal.
- PCRE2 may still be pulled by another embedded object. The dependency check is
  therefore part of acceptance, not an assumption.
