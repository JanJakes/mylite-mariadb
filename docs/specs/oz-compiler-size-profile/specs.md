# Oz Compiler Size Profile

## Problem Statement

The current aggressive minsize profile still uses CMake's default
`MinSizeRel` optimization level, which GCC records as `-Os -DNDEBUG`.
GCC 13 in the MyLite build container also accepts `-Oz`, a stronger size
optimization mode. This slice tests whether switching only the minsize build
type flags to `-Oz` reduces the linked embedded runtime without changing SQL
or storage semantics.

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/build-mariadb-minsize.sh` owns the reproducible aggressive embedded
  build profile and currently uses `CMAKE_BUILD_TYPE=MinSizeRel`.
- The current `build/mariadb-minsize-no-load-data/CMakeCache.txt` records
  `CMAKE_C_FLAGS_MINSIZEREL=-Os -DNDEBUG` and
  `CMAKE_CXX_FLAGS_MINSIZEREL=-Os -DNDEBUG`.
- The Ubuntu 24.04 build container's GCC/G++ 13.3.0 accepts `-Oz` for C and
  C++ compilation.
- Existing minsize source changes already use section GC, lld RELR, and ICF.
  This slice therefore measures a compiler-code-generation lever on top of the
  current source-pruned baseline.

## Proposed Design

Set the MyLite minsize CMake build-type flags explicitly:

```text
CMAKE_C_FLAGS_MINSIZEREL=-Oz -DNDEBUG
CMAKE_CXX_FLAGS_MINSIZEREL=-Oz -DNDEBUG
```

Keep the change local to `tools/build-mariadb-minsize.sh`; do not change
MariaDB source semantics or non-minsize builds. Also record these cache entries
in `mylite-build-report.txt` so future size reports show the active compiler
optimization level.

## Affected Subsystems

- Build tooling and compiler optimization flags for the aggressive minsize
  profile.
- Generated machine code and possibly runtime performance.
- No intended SQL, storage, public API, catalog, or file-format change.

## Single-File And Embedded-Lifecycle Impact

None expected. This changes compiler optimization only.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Pre-change baseline from `build/mariadb-minsize-no-load-data`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 29,169,370 |
| `mylite/mylite-open-close-smoke` | 7,755,752 |
| stripped `mylite-open-close-smoke` copy | 5,570,344 |
| stripped `mylite-compatibility-smoke` copy | 5,462,704 |

Measured against `build/mariadb-minsize-oz`:

| Artifact | No-LOAD-DATA profile | `-Oz` profile | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,169,370 | 29,169,370 | 0 |
| `mylite/libmylite.a` | 122,792 | 122,784 | -8 |
| `storage/mylite/libmylite_embedded.a` | 388,440 | 388,440 | 0 |
| `mylite-open-close-smoke` | 7,755,752 | 7,755,552 | -200 |
| stripped `mylite-open-close-smoke` copy | 5,570,344 | 5,570,216 | -128 |
| `mylite-compatibility-smoke` | 7,627,040 | 7,627,040 | 0 |
| stripped `mylite-compatibility-smoke` copy | 5,462,704 | 5,462,704 | 0 |

The linked open-close section profile changed only marginally:

| Section group | No-LOAD-DATA profile | `-Oz` profile | Delta |
| --- | ---: | ---: | ---: |
| text | 4,388,752 | 4,388,616 | -136 |
| data | 1,178,232 | 1,178,232 | 0 |
| bss | 229,737 | 229,865 | +128 |
| total `size` decimal | 5,796,721 | 5,796,713 | -8 |

The useful shipped-size signal is therefore only 128 bytes in the stripped
open-close executable. This is a measurable low-level win, but not a meaningful
feature-pruning lever.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Also measure:

- static archive size,
- unstripped and stripped linked smoke binaries,
- section profile from `size`,
- largest linked sections with `size -A`.

## Acceptance Criteria

- The minsize build completes with `-Oz` recorded in CMake cache and build
  report.
- Embedded bootstrap, open/close, and compatibility smokes pass.
- Size deltas are recorded in this spec and in
  `docs/research/production-size-analysis.md`.
- If `-Oz` does not reduce useful shipped-size signals, reject the profile
  change and record why.

## Risks And Unresolved Questions

- `-Oz` may trade execution speed for smaller code. That is acceptable only in
  the aggressive minsize profile.
- Compiler code generation can shift linked-section behavior; the archive and
  linked artifacts must be measured separately.
- This does not remove any SQL feature by itself, so large parser/type objects
  will remain dominant after the experiment.

## Implementation Result

Implemented in `tools/build-mariadb-minsize.sh` by overriding the `MinSizeRel`
C and C++ flags to `-Oz -DNDEBUG` and by recording those cache entries in the
generated build report.

Verification run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-oz MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh`
- `bash -n tools/build-mariadb-minsize.sh`
