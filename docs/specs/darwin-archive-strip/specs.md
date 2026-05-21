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
- Apple `strip(1)` accepts `-S -x` for debug/local-symbol stripping and
  documents `-u -r` as the maximum strip level for a dynamically linked
  executable that still runs with its libraries. Applied together to the
  static archive, the combined mode preserves enough external symbol
  information for the current `libmylite` embedded build to relink.
- The vector-trim profile used to correct the clean-build strip policy
  measured:

  | Mode | Bytes |
  | --- | ---: |
  | Pre-strip archive | 26,496,392 |
  | Darwin `strip -S -x -u -r` archive | 25,937,816 |

- The current embedded build links against the `strip -S -x -u -r` archive and
  passes all embedded tests.

## Design

Keep archive stripping enabled by default. On Darwin, strip the archive with
`strip -S -x -u -r`; on other platforms, keep the existing `strip -S -x`
behavior until platform-specific relink evidence exists.

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

That macOS embedded archive changed from 26,496,392 bytes to 25,937,816 bytes,
saving 558,576 bytes with no archive-member count change.

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

- rebuild the embedded archive from a deleted generated archive and marker;
- run all embedded tests against the stripped archive; and
- confirm `tools/mariadb-embedded-build measure` reports
  `strip_signature=strip-v3:darwin-debug-local-undefined-and-dynamic`.

## Acceptance Criteria

- Darwin builds use the relink-verified strip mode.
- Non-Darwin builds keep the existing strip mode.
- The marker records the strip signature and re-strips when the signature
  changes.
- The measured archive size and member count are recorded.
- Embedded and first-party tests, format, tidy, and diff checks pass.
