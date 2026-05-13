# Clang PIC Size Profile

## Problem Statement

The current aggressive minsize profile is built with GCC/G++ and can produce a
PHP-extension-shaped shared-object probe of about 3.89 MiB stripped. This slice
tests whether Clang can reduce the final linked artifact size without changing
SQL semantics.

The shared-object shape matters because it is closer to how a PHP extension
would ship MyLite than the static `libmariadbd.a` archive.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `tools/build-mariadb-minsize.sh` owns the aggressive GCC minsize profile.
- The existing profile builds static MariaDB/MyLite archives and then relies on
  section GC and ICF when linking executable or shared-library style artifacts.
- A Clang 18 build without PIC passed the open/close smoke and produced a
  smaller executable, but the shared-object probe failed with
  `R_AARCH64_TLSLE_*` relocations against thread-local MariaDB/MyLite symbols.
- Rebuilding with `-DWITH_PIC=ON` and `-ftls-model=global-dynamic` allows the
  shared-object probe to link.

## Design

Measure Clang as a packaging/toolchain candidate only:

- keep the current source-pruned minsize feature stack unchanged,
- use Clang/Clang++ 18 from Ubuntu 24.04,
- keep `MinSizeRel` flags at `-Oz -DNDEBUG`,
- add `-DWITH_PIC=ON`,
- add `-ftls-model=global-dynamic` for C and C++ compilation, and
- link the PHP-shaped probe with the same lld RELR, no-EH-frame-header,
  section-GC, ICF, RELRO, and version-script policy used by the GCC probe.

This slice does not switch the default build script to Clang. Toolchain
availability, x86-64 measurements, and release packaging policy need a separate
decision.

## Non-Goals

- Do not remove SQL functionality.
- Do not change the public MyLite C API or `.mylite` file format.
- Do not treat static archive size as the shipping signal for PHP-style
  packaging.
- Do not require Clang in the default minsize Docker image yet.

## Binary-Size Impact

Current GCC baseline from `build/mariadb-minsize-server-account-sql`:

| Artifact | GCC bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 22,437,702 |
| `mylite/libmylite.a` | 76,696 |
| `storage/mylite/libmylite_embedded.a` | 388,456 |
| unstripped `mylite-open-close-smoke` | 5,860,688 |
| stripped `mylite-open-close-smoke` | 3,995,560 |
| stripped minimal executable probe | 3,886,264 |
| stripped PHP-shaped shared-object probe | 3,886,256 |

Measured Clang/PIC profile from `build/mariadb-minsize-clang-pic`:

| Artifact | Clang/PIC bytes | Delta vs GCC |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 26,823,772 | +4,386,070 |
| `mylite/libmylite.a` | 106,064 | +29,368 |
| `storage/mylite/libmylite_embedded.a` | 492,624 | +104,168 |
| unstripped `mylite-open-close-smoke` | 6,163,184 | +302,496 |
| stripped `mylite-open-close-smoke` | 3,880,384 | -115,176 |
| stripped minimal executable probe | 3,788,152 | -98,112 |
| stripped PHP-shaped shared-object probe | 3,787,616 | -98,640 |

The static archives are larger with Clang/PIC, but the final stripped linked
artifacts are smaller. For PHP-extension-style packaging, the useful signal is
the stripped shared-object probe: 3,787,616 bytes, about 0.09 MiB smaller than
the current GCC probe.

The Clang/PIC linked open-close smoke section profile is:

| Section group | Bytes | Delta vs GCC |
| --- | ---: | ---: |
| text | 2,916,522 | -101,569 |
| data | 960,536 | -13,648 |
| bss | 215,505 | +392 |
| total `size` decimal | 4,092,563 | -114,825 |

The earlier non-PIC Clang build remains rejected for PHP/shared packaging
because its static objects use local-exec TLS relocations that cannot be linked
into a shared object.

## Test And Verification Plan

Build in an Ubuntu 24.04 minsize container with Clang installed. This is not
yet first-class build-script behavior; the measurement reused the generated
minsize CMake command and appended `-DWITH_PIC=ON`.

```sh
CC=clang CXX=clang++ \
  CFLAGS="-ftls-model=global-dynamic" \
  CXXFLAGS="-ftls-model=global-dynamic" \
  cmake -S vendor/mariadb/server \
    -B build/mariadb-minsize-clang-pic \
    <current minsize CMake arguments> \
    -DWITH_PIC=ON

cmake --build build/mariadb-minsize-clang-pic \
  --target mysqlserver --parallel 8
```

Then run the broader smokes against the same build directory:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  tools/run-libmylite-open-close-smoke.sh --inside-container
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  tools/run-embedded-bootstrap-smoke.sh --inside-container
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  tools/run-storage-engine-smoke.sh --inside-container
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  tools/run-compatibility-test-harness.sh --inside-container
```

Inside the container, the smoke commands were run with:

```sh
CC=clang
CXX=clang++
CFLAGS="-ftls-model=global-dynamic"
CXXFLAGS="-ftls-model=global-dynamic"
```

Also link and strip:

- a minimal executable probe that calls `mylite_open()` and `mylite_close()`,
- a shared-object probe that exports one wrapper symbol through a version
  script.

Before making this a default or script-supported profile, add a proper
`tools/build-mariadb-minsize.sh` option for PIC and compiler selection instead
of relying on a manual CMake rerun.

## Verification

Passed:

```sh
CC=clang CXX=clang++ \
  CFLAGS="-ftls-model=global-dynamic" \
  CXXFLAGS="-ftls-model=global-dynamic" \
  cmake --build build/mariadb-minsize-clang-pic \
    --target mysqlserver --parallel 8

CC=clang CXX=clang++ \
  CFLAGS="-ftls-model=global-dynamic" \
  CXXFLAGS="-ftls-model=global-dynamic" \
  MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  MYLITE_BUILD_JOBS=8 \
  tools/run-embedded-bootstrap-smoke.sh --inside-container

CC=clang CXX=clang++ \
  CFLAGS="-ftls-model=global-dynamic" \
  CXXFLAGS="-ftls-model=global-dynamic" \
  MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  MYLITE_BUILD_JOBS=8 \
  tools/run-storage-engine-smoke.sh --inside-container

CC=clang CXX=clang++ \
  CFLAGS="-ftls-model=global-dynamic" \
  CXXFLAGS="-ftls-model=global-dynamic" \
  MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-clang-pic \
  MYLITE_BUILD_JOBS=8 \
  tools/run-compatibility-test-harness.sh --inside-container
```

Open/close smoke also passed before the broader smoke set.

The PHP-shaped shared-object probe linked successfully and depended only on
`libstdc++`, `libm`, `libgcc_s`, and `libc`.

## Decision

Keep Clang/PIC as a candidate packaging profile. It is the lowest measured
PHP-shaped shared-object size so far. Do not switch the default profile yet:
the archive bloat is substantial, the Docker image does not currently include
Clang, and x86-64 needs its own measurement before making a broad release
claim.
