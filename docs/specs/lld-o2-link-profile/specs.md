# LLD O2 Link Profile

## Problem Statement

The aggressive minsize profile already links runtime-style artifacts with lld,
RELR relocation packing, section GC, and identical code folding. The profile
did not explicitly request lld's higher link-time optimization level. This
slice tests whether `-Wl,-O2` reduces final linked artifacts without changing
compiled objects or SQL behavior.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `tools/build-mariadb-minsize.sh` builds runtime-style artifacts with lld.
- The current linker flags already include RELR, no `.eh_frame_hdr`, section
  GC, and `--icf=all`.
- A link-only sweep against the current GCC and Clang/PIC PHP-shaped shared
  probes showed `-Wl,-O2` saves about 4.3 KiB.
- Dropping build-id, lazy binding, or RELRO saved little or created worse
  release/debug/security tradeoffs.

## Design

Add `-Wl,-O2` to the common minsize linker flags in
`tools/build-mariadb-minsize.sh`:

```text
-fuse-ld=lld -Wl,-O2 ...
```

Apply it consistently to executable, module, and shared linker flags. Do not
change compiler flags, MariaDB sources, MyLite sources, public API, or storage
format.

## Binary-Size Impact

Measured against `build/mariadb-minsize-server-account-sql`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 22,437,702 | 22,437,702 | 0 |
| `mylite/libmylite.a` | 76,696 | 76,680 | -16 |
| `storage/mylite/libmylite_embedded.a` | 388,456 | 388,456 | 0 |
| unstripped `mylite-open-close-smoke` | 5,860,688 | 5,855,640 | -5,048 |
| stripped `mylite-open-close-smoke` | 3,995,560 | 3,990,440 | -5,120 |
| sectionless `mylite-open-close-smoke` | 3,993,128 | 3,988,008 | -5,120 |
| unstripped minimal executable probe | 5,734,328 | 5,729,976 | -4,352 |
| stripped minimal executable probe | 3,886,264 | 3,881,912 | -4,352 |
| unstripped PHP-shaped shared-object probe | 5,734,144 | 5,729,736 | -4,408 |
| stripped PHP-shaped shared-object probe | 3,886,256 | 3,881,776 | -4,480 |
| sectionless PHP-shaped shared-object probe | 3,883,896 | 3,879,416 | -4,480 |

The current linked open-close smoke section profile after `-Wl,-O2` is:

| Section group | Bytes | Delta |
| --- | ---: | ---: |
| text | 3,012,979 | -5,112 |
| data | 974,184 | 0 |
| bss | 216,137 | +1,024 |
| total `size` decimal | 4,203,300 | -4,088 |

On the opt-in Clang/PIC PHP-shaped shared-object probe, link-only measurement
with `-Wl,-O2` reduced the stripped artifact from 3,787,616 to 3,783,264
bytes, and the sectionless artifact from 3,785,216 to 3,780,864 bytes.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Also link and strip minimal executable and PHP-shaped shared-object probes with
the same linker flags.

## Verification

Passed:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-lld-o2 MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh`

## Decision

Keep `-Wl,-O2` in the aggressive minsize profile. It is a small but clean
linker-only win, keeps RELRO and build-id intact, and does not change SQL or
storage behavior.
