# EH Frame Header Size Profile

## Problem Statement

The current aggressive minsize profile still emits a linked `.eh_frame_hdr`
section and `PT_GNU_EH_FRAME` program header. In
`build/mariadb-minsize-compact-errors/mylite/mylite-open-close-smoke`, that
section is 96,772 bytes. This slice tests whether the runtime-style linked
artifact can omit the header while keeping C++ exception support and current
MyLite behavior.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant local source paths:

- `tools/build-mariadb-minsize.sh` owns the aggressive minsize linker flags.
- `vendor/mariadb/server/CMakeLists.txt` owns MyLite-specific minsize options
  such as section GC and reduced unwind tables.
- `docs/specs/unwind-table-size-profile/specs.md` records the earlier
  `-fno-asynchronous-unwind-tables -fno-unwind-tables` experiment, which kept
  exception support and still left `.eh_frame_hdr` present.
- The current linked section profile includes:
  - `.eh_frame`: 453,928 bytes,
  - `.eh_frame_hdr`: 96,772 bytes,
  - `.gcc_except_table`: 35,100 bytes.

Toolchain evidence from the Ubuntu 24.04 minsize container:

```text
ld.lld --help
  --eh-frame-hdr          Request creation of .eh_frame_hdr section and PT_GNU_EH_FRAME segment header
  --no-eh-frame-hdr       Do not create .eh_frame_hdr section
```

## Design

Add a MyLite minsize option named `MYLITE_DISABLE_EH_FRAME_HEADER`.

When enabled, `tools/build-mariadb-minsize.sh` adds:

```text
-Wl,--no-eh-frame-hdr
```

to executable, module, and shared linker flags. The default aggressive minsize
profile enables it because the active size goal is to get linked artifacts as
small as practical and commit attempts for later keep/drop review.

The option only removes the generated ELF lookup header. It must not add
`-fno-exceptions`, and it must not remove `.eh_frame` or
`.gcc_except_table`, because those are still needed for retained C++ exception
support.

## Non-Goals

- Do not remove more SQL syntax, functions, storage engines, or runtime
  subsystems in this slice.
- Do not claim static archive savings.
- Do not claim this is automatically the safest broad-distribution default.
- Do not change the public MyLite C API or `.mylite` file format.

## Affected Subsystems

- Minsize build tooling.
- Linked executable, shared-library, and module artifacts produced by the
  minsize profile.

No SQL execution, storage, parser, public API, or file-format behavior is
intended to change.

## DDL Metadata Routing Impact

None. This is a link-format change only.

## Single-File And Embedded-Lifecycle Impact

None. The change does not affect catalog storage, row/index storage, locks,
recovery, sidecars, startup, open/close semantics, or runtime ownership.

## Public API Or File-Format Impact

No public ABI source declaration or `.mylite` file-format change.

Runtime ABI behavior changes only in the sense that linked ELF artifacts no
longer publish a compact unwind lookup header for their own `.eh_frame`
entries. Current smokes and the compatibility harness must prove the result
still runs under the supported Linux container.

## Binary-Size Impact

Expected impact:

- stripped linked runtime-style artifact: roughly the current `.eh_frame_hdr`
  section size, about 95 KiB, minus ELF layout noise,
- static embedded archive: no expected change,
- dynamic library dependencies: no expected change.

The current pre-slice linked section profile from
`build/mariadb-minsize-compact-errors` is:

| Section | Bytes |
| --- | ---: |
| `.eh_frame` | 453,928 |
| `.eh_frame_hdr` | 96,772 |
| `.gcc_except_table` | 35,100 |

Measured implementation impact against
`build/mariadb-minsize-compact-errors`:

| Artifact | Compact errors | No EH frame header | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 26,484,414 | 26,484,414 | 0 |
| `mylite/libmylite.a` | 122,792 | 122,800 | +8 |
| `storage/mylite/libmylite_embedded.a` | 388,456 | 388,456 | 0 |
| `mylite/mylite-open-close-smoke` | 6,965,448 | 6,868,688 | -96,760 |
| stripped `mylite-open-close-smoke` copy | 4,938,992 | 4,842,168 | -96,824 |

The linked no-EH-header smoke section profile is:

| Section group | Bytes |
| --- | ---: |
| text | 3,814,505 |
| data | 1,024,504 |
| bss | 225,249 |
| total `size` decimal | 5,064,258 |

`readelf -S` confirms `.eh_frame` and `.gcc_except_table` remain present while
`.eh_frame_hdr` is absent. `readelf -l` confirms there is no
`GNU_EH_FRAME` program header.

## License, Trademark, And Dependency Impact

No new runtime or build dependency. The profile already uses LLVM `lld`.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

```sh
llvm-size -A build/mariadb-minsize-no-eh-frame-header/mylite/mylite-open-close-smoke
cp build/mariadb-minsize-no-eh-frame-header/mylite/mylite-open-close-smoke \
  build/mariadb-minsize-no-eh-frame-header/mylite/mylite-open-close-smoke.stripped
llvm-strip build/mariadb-minsize-no-eh-frame-header/mylite/mylite-open-close-smoke.stripped
```

## Acceptance Criteria

- The minsize build completes.
- The linker flags include `-Wl,--no-eh-frame-hdr`.
- `.eh_frame_hdr` is absent from the linked smoke binary.
- `.eh_frame` and `.gcc_except_table` remain present.
- Open/close, storage-engine, embedded-bootstrap, and grouped compatibility
  smokes pass.
- The size delta is recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- Removing `.eh_frame_hdr` can slow exception unwinding and stack-unwind
  lookup for linked artifacts. It should be treated as an aggressive minsize
  packaging choice until release deployment requirements are known.
- This does not shrink `libmariadbd.a`; it only affects final linked
  executable/shared/module outputs.
- The current measurement is Linux/aarch64/glibc. Other platforms need their
  own measurement before making packaging promises.

## Implementation Result

Implemented by:

- adding `MYLITE_DISABLE_EH_FRAME_HEADER` to the aggressive minsize build
  script and MariaDB CMake cache metadata,
- defaulting the aggressive minsize profile to `-Wl,--no-eh-frame-hdr`,
- preserving `MYLITE_DISABLE_EH_FRAME_HEADER=OFF` for comparison builds.

Verification run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-eh-frame-header \
  tools/run-compatibility-test-harness.sh
```

Observed evidence:

- `build/mariadb-minsize-no-eh-frame-header/mylite-build-report.txt` records
  `MYLITE_DISABLE_EH_FRAME_HEADER:BOOL=ON`.
- Executable, module, and shared linker flags include
  `-Wl,--no-eh-frame-hdr`.
- `libmylite-open-close-report.txt` and the grouped compatibility harness
  report `status=0`.
