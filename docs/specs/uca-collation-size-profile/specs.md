# UCA collation size profile

## Problem

The current aggressive minsize profile still links MariaDB's UCA collation
tables. After the server-utility function slice, the largest non-parser
objects remaining in the embedded archive include:

- `ctype-uca1400.c.o`: 1,009,256 bytes
- `ctype-uca.c.o`: 747,272 bytes
- `ctype-uca0900.c.o`: 10,064 bytes

The linked smoke binary also carries UCA data symbols such as
`my_uca1400_info_tailored`, `my_uca_v1400`, `my_uca1400_collation_definitions`,
and `mysql_0900_mapping`.

This slice tests whether the most aggressive MyLite profile can omit UCA
collations entirely and use `utf8mb4_general_ci` as the compiled default.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Official MariaDB documentation records the current default character set as
`utf8mb4` and the current default collation as `utf8mb4_uca1400_ai_ci`:

- <https://mariadb.com/docs/server/reference/data-types/string-data-types/character-sets/setting-character-sets-and-collations>
- <https://mariadb.com/docs/server/reference/data-types/string-data-types/character-sets/supported-character-sets-and-collations>

Relevant source paths:

- `vendor/mariadb/server/cmake/character_sets.cmake` defaults
  `DEFAULT_COLLATION` to `utf8mb4_uca1400_ai_ci` and always sets
  `HAVE_UCA_COLLATIONS=1`.
- `vendor/mariadb/server/strings/CMakeLists.txt` always compiles
  `ctype-uca.c`, `ctype-uca0900.c`, and `ctype-uca1400.c`, and always builds
  the generated `ctype-uca1400data.h` target.
- `vendor/mariadb/server/mysys/charset-def.c:init_compiled_charsets()` guards
  legacy compiled UCA 5.2.0 collation declarations and additions with
  `HAVE_UCA_COLLATIONS`, but unconditionally calls
  `my_uca1400_collation_definitions_add()`,
  `mysql_uca0900_utf8mb4_collation_definitions_add()`, and
  `mysql_utf8mb4_0900_bin_add()`.
- `vendor/mariadb/server/sql/mysqld.cc` initializes
  `character_set_collations_str` to map Unicode character sets to
  `uca1400_ai_ci`.
- `vendor/mariadb/server/strings/ctype-uca.c` wraps its UCA implementation with
  `HAVE_UCA_COLLATIONS`, but `ctype-uca0900.c` and `ctype-uca1400.c` do not.
- `vendor/mariadb/server/sql/lex_charset.cc` still parses contextual
  `uca1400_*` collation names. With UCA omitted, those names should fail
  through the normal collation lookup path.

## Design

Add `MYLITE_DISABLE_UCA_COLLATIONS` as a minsize-only CMake profile switch.
When enabled:

- do not define `HAVE_UCA_COLLATIONS`,
- require the caller to choose a non-UCA `DEFAULT_COLLATION`,
- omit `ctype-uca.c`, `ctype-uca0900.c`, and `ctype-uca1400.c` from the
  `strings` library,
- skip the generated UCA 1400 data target, and
- leave `character_set_collations_str` empty so startup does not remap Unicode
  character sets to unavailable `uca1400_ai_ci` context collations.

Set MyLite's aggressive minsize build to:

```text
MYLITE_DISABLE_UCA_COLLATIONS=ON
DEFAULT_COLLATION=utf8mb4_general_ci
```

This deliberately trades MariaDB 11.8 default collation compatibility for a
smaller binary. It should stay an aggressive size attempt until product
compatibility policy decides whether MariaDB's UCA 1400 defaults are required.

## Non-Goals

- Do not remove `utf8mb4`, `utf8mb3`, `latin1`, or binary collations.
- Do not change SQL grammar for `COLLATE`; unavailable UCA names should fail
  through inherited MariaDB collation diagnostics.
- Do not remove PCRE, JSON, date/time, parser, optimizer, or execution code in
  this slice.
- Do not claim that the default MyLite product should use `utf8mb4_general_ci`;
  this is an aggressive minsize experiment.

## Compatibility Impact

High. MariaDB 11.8 defaults to UCA 1400 for `utf8mb4`, while this profile uses
`utf8mb4_general_ci`. Sorting, comparison, unique-key behavior, explicit
`COLLATE uca1400_*`, MySQL 8.0 `utf8mb4_0900_*` collation aliases, and
information-schema collation inventory can change.

Retained expected behavior:

- embedded startup succeeds,
- `@@collation_server` is `utf8mb4_general_ci`,
- `SELECT 'a' COLLATE utf8mb4_general_ci` succeeds,
- `SELECT 'a' COLLATE utf8mb4_uca1400_ai_ci` fails with MariaDB's normal
  collation diagnostic, and
- basic scalar, DDL, DML, storage, and compatibility harness smokes still pass.

## Single-File and Embedded-Lifecycle Impact

No file-format, catalog, locking, journaling, or sidecar behavior should
change. The affected lifecycle point is embedded startup and charset/collation
initialization.

## Binary-Size Impact

Expected archive savings are at least most of the current sizes of the UCA
string objects, about 1.69 MiB. Linked savings should be smaller than the
archive delta because section garbage collection already drops unreferenced
pieces, but the current linked binary still contains UCA data symbols and
should shrink.

Measure:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

Compare the archive and stripped linked smoke against
`build/mariadb-minsize-server-utility-functions`.

Measured on 2026-05-12:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 35,555,602 | 33,777,694 | -1,777,908 |
| archive object count | 460 | 458 | -2 |
| `mylite/libmylite.a` | 122,800 | 122,792 | -8 |
| `storage/mylite/libmylite_embedded.a` | 388,440 | 388,440 | 0 |
| unstripped `mylite-open-close-smoke` | 10,838,176 | 9,255,608 | -1,582,568 |
| stripped `mylite-open-close-smoke` | 8,318,304 | 6,765,440 | -1,552,864 |
| `size` total | 8,612,773 | 7,016,073 | -1,596,700 |

The retained `ctype-uca.c.o` no-UCA helper object is 2,480 bytes in the
stripped archive. `ctype-uca0900.c.o` and `ctype-uca1400.c.o` are absent.

## Test Plan

Add open/close smoke coverage for:

- `@@collation_server`,
- successful `utf8mb4_general_ci` collation use, and
- unsupported `utf8mb4_uca1400_ai_ci` use in the aggressive minsize profile.

Then run the commands listed in the binary-size section and `git diff --check`.

## Verification

Run on 2026-05-12:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-uca-collations \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

All passed. The compatibility sidecar scan reported no unexpected sidecars.
The linked smoke binary no longer contains `my_uca1400_*`, `my_uca_v1400`, or
`mysql_0900_mapping` symbols. `build/mariadb-minsize-uca-collations/include/config.h`
sets `MYSQL_DEFAULT_COLLATION_NAME` to `utf8mb4_general_ci` and leaves
`HAVE_UCA_COLLATIONS` undefined.

## Acceptance Criteria

- The minsize build completes with `MYLITE_DISABLE_UCA_COLLATIONS=ON`.
- The linked smoke binary has no `my_uca1400_*`, `my_uca_v1400`, or
  `mysql_0900_mapping` symbols.
- Current smoke and compatibility harness tests pass.
- Measured archive and stripped linked binary deltas are recorded here and in
  `docs/research/production-size-analysis.md`.
- The compatibility tradeoff is documented as high risk.

## Risks

- Some startup or SQL paths may assume contextual `uca1400_*` collations exist.
- Collation-dependent comparisons can diverge from MariaDB 11.8 defaults.
- MySQL 8.0 replication/migration compatibility aliases are removed with UCA
  0900 support.
- If future MyLite catalog data stores collation IDs created under this profile,
  profile changes must include migration rules.
