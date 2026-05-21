# Darwin Archive Strip

## Problem Statement

The embedded build wrapper stripped debug and local symbols from
`libmariadbd.a` with `strip -S -x`. That was safe, but on Darwin it left
additional relink-unneeded symbol-table payload in the distributed static
archive. The wrapper also used a timestamp-only `.mylite-stripped` marker, so a
future strip policy change could be skipped when the archive was older than an
old marker.

This slice should reduce packaged archive size without removing SQL,
storage-engine, JSON, GEOMETRY/GIS, charset, collation, or API functionality.

## Source Findings

- `tools/mariadb-embedded-build` owns the post-build archive strip step and
  refreshes the archive index with `ranlib`.
- Apple `strip(1)` documents `-u -r` as the maximum strip level for a
  dynamically linked executable that still runs with its libraries. Applied to
  the copied static archive, it preserves enough external symbol information
  for the current `libmylite` embedded build to relink.
- A copied current archive measured:

  | Mode | Bytes |
  | --- | ---: |
  | Existing `strip -S -x` archive | 26,028,560 |
  | Darwin `strip -u -r` archive | 26,020,528 |

- A scratch embedded build linked against the copied `strip -u -r` archive and
  passed all 14 embedded tests.

## Design

Keep archive stripping enabled by default. On Darwin, strip the archive with
`strip -u -r`; on other platforms, keep the existing `strip -S -x` behavior
until platform-specific relink evidence exists.

Record a strip signature in `libmariadbd.a.mylite-stripped`. The wrapper skips
re-stripping only when the archive is older than the marker and the marker
signature matches the current strip policy. `measure` prints the signature when
present.

`STRIP_ARCHIVE=0` remains the developer escape hatch for unstripped local
inspection.

## Compatibility Impact

This is packaging-only. It does not remove MariaDB source files, change build
feature toggles, alter runtime behavior, or change the public `libmylite` API.
The acceptance test is relinking and running the embedded test suite from a
copy of the more aggressively stripped archive.

## Binary-Size Impact

The current macOS embedded archive changes from 26,028,560 bytes to 26,020,528
bytes, saving 8,032 bytes with no archive-member count change. The full
post-build strip step now saves 567,616 bytes from the current pre-strip
archive.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

Additional verification:

- build a scratch embedded tree against a copied `strip -u -r` archive;
- run all embedded tests in the scratch tree; and
- confirm `tools/mariadb-embedded-build measure` reports
  `strip_signature=strip-v2:darwin-undefined-and-dynamic`.

## Acceptance Criteria

- Darwin builds use the relink-verified strip mode.
- Non-Darwin builds keep the existing strip mode.
- The marker records the strip signature and re-strips when the signature
  changes.
- The measured archive size and member count are recorded.
- Embedded and first-party tests, format, tidy, and diff checks pass.
