# Embedded Default Files Size Profile

## Problem Statement

The aggressive MyLite minsize profile starts the embedded MariaDB runtime from a
fully controlled argument vector that places `--no-defaults` immediately after
the program name. Despite that, `init_embedded_server()` still calls MariaDB's
option-file loader, which keeps default-file parsing and search code linked
into the MyLite runtime.

MyLite should not read host `my.cnf` files or environment-selected option groups
when opening an application-owned `.mylite` file. Bypassing this path in the
aggressive embedded profile removes daemon configuration compatibility that
`libmylite` does not expose while preserving SQL behavior.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/mylite/mylite.cc:1272` builds the current
  `libmylite` server argument vector and passes `--no-defaults` immediately
  after the program name.
- `vendor/mariadb/server/mylite/bootstrap_smoke.cc:117` and
  `vendor/mariadb/server/mylite/storage_engine_smoke.cc:313` use the same
  controlled startup pattern for embedded test programs.
- `vendor/mariadb/server/libmysqld/lib_sql.cc:546` implements
  `init_embedded_server()` and always calls `load_defaults("my", ...)` before
  option parsing.
- `vendor/mariadb/server/mysys/my_default.c:407` implements
  `my_load_defaults()`. Even with `--no-defaults`, the loader initializes
  default-directory state and constructs a replacement argv array.
- `vendor/mariadb/server/sql/mysqld.cc:2045` frees `defaults_argv` with
  `free_defaults()` during cleanup, so a non-`load_defaults()` argv path must
  avoid assigning `defaults_argv`.

## Design

Add `MYLITE_DISABLE_EMBEDDED_DEFAULT_FILES`.

When enabled in `libmysqld`:

- `init_embedded_server()` requires the supplied argv to have `--no-defaults`
  immediately after the program name, matching MariaDB's `load_defaults()`
  convention.
- If the requirement is met, it copies the provided argv with the existing
  `copy_arguments()` helper, sets `remaining_argc` and `remaining_argv` to that
  copy, and leaves `defaults_argv` as `nullptr` so `clean_up()` does not call
  `free_defaults()` on memory not allocated by `load_defaults()`.
- If the requirement is not met, startup fails early with an explicit embedded
  configuration error.

This keeps the retained option parser and all existing command-line options used
by MyLite startup. It removes host option-file scanning only.

## Non-Goals

- Do not remove `handle_options()` or MariaDB system-variable command-line
  parsing.
- Do not remove any SQL statements, parser actions, optimizer behavior, storage
  behavior, diagnostics, or type semantics.
- Do not change the public `libmylite` API or the `.mylite` file format.
- Do not make this behavior the generic MariaDB embedded default outside the
  MyLite minsize profile.

## Binary-Size Impact

Expected savings are small but legitimate: the linked runtime currently still
contains `get_defaults_options()` and `my_load_defaults()`, plus supporting
default-file code. The archive impact may be larger if the bypass lets section
GC drop more of `my_default.c`.

This is worth testing because it is a pure embedded-lifecycle cut with no SQL
compatibility loss.

Implemented measurements against `build/mariadb-minsize-no-tpool`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,391,070 | 25,391,134 | +64 |
| `mylite/libmylite.a` | 76,122 | 76,130 | +8 |
| `storage/mylite/libmylite_embedded.a` | 388,456 | 388,456 | 0 |
| unstripped `mylite-open-close-smoke` | 6,485,584 | 6,479,480 | -6,104 |
| stripped `mylite-open-close-smoke` | 4,551,888 | 4,546,760 | -5,128 |
| `size` decimal total | 4,774,464 | 4,769,232 | -5,232 |

The linked open-close smoke no longer contains `my_load_defaults()` or
`get_defaults_options()`. The archive grows slightly because the existing
`copy_arguments()` helper is now retained in `lib_sql.cc.o`.

## Test And Verification Plan

Build and run the current smoke set:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  tools/run-compatibility-test-harness.sh
git diff --check
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
```

Measure:

- `libmysqld/libmariadbd.a`,
- unstripped `mylite-open-close-smoke`,
- stripped `mylite-open-close-smoke`,
- `llvm-size` section totals, and
- absence or retention of `my_load_defaults` and `get_defaults_options`.

## Acceptance Criteria

- The minsize build enables `MYLITE_DISABLE_EMBEDDED_DEFAULT_FILES=ON`.
- MyLite startup still succeeds from the controlled `--no-defaults` argv.
- Current embedded, open/close, storage, and compatibility smokes pass.
- The linked open-close smoke no longer contains `my_load_defaults()` when the
  linker can drop it.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-default-files \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

The open-close smoke reported:

```text
libmylite default-file symbols: none
```
