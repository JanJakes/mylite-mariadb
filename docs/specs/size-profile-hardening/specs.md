# Size Profile Hardening

## Problem Statement

MyLite should start reducing binary size only where the change does not remove
important MySQL/MariaDB behavior. The first safe steps are packaging hygiene
and omission of already-disabled server surfaces: remove debug and local-symbol
metadata from the embedded static archive after the normal MariaDB build, and
avoid building the unused Performance Schema static plugin.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current measured archive before this slice:
  `build/mariadb-embedded/libmysqld/libmariadbd.a`, 33,842,320 bytes,
  32.27 MiB, 822 members.
- The largest original archive members include charset/collation tables,
  generated parsers, core SQL item files, JSON, GEOMETRY/GIS, and native engine
  code. Those are not safe first cuts because they remove observable SQL,
  type, collation, or storage behavior.
- Performance Schema accounts for about 1.28 MiB and 112 archive members in
  the symbol-stripped archive. It is already outside the default embedded
  profile, can be disabled at startup when present, and is covered by
  server-surface tests as omitted or disabled.
- Historical bundle-size research shows archive symbol stripping as a
  packaging-only reduction that passed relinked smokes. The old `strip -g`
  command is GNU-specific; Apple `strip` accepts `-S -x` for debug/local-symbol
  stripping.
- On the current macOS baseline, `strip -S -x` plus `ranlib` on a copy of
  `libmariadbd.a` reduces the archive by 712,680 bytes without changing archive
  membership.
- Setting `PLUGIN_PERFSCHEMA=NO` and keeping archive stripping enabled reduces
  the current archive to 31,529,704 bytes, 30.07 MiB, and 712 members.

## Proposed Design

After building the embedded archive, `tools/mariadb-embedded-build` strips
debug and local symbols from `libmariadbd.a` and refreshes the archive index
with `ranlib`.

The embedded baseline also disables the Performance Schema storage engine at
configure time. The runtime only passes `--performance-schema=OFF` when the
MariaDB build exposes that option, preserving the explicit disabled
server-surface contract for custom builds while avoiding the unused static
Performance Schema archive members in the default profile.

The wrapper keeps this behavior enabled by default because it is the
distributed archive profile. Developers can set `STRIP_ARCHIVE=0` when they
need an unstripped archive for local inspection.

## Affected MariaDB Subsystems

No MariaDB source files are changed. The Performance Schema storage-engine
plugin is omitted by CMake configuration.

## Compatibility Impact

No application compatibility impact is expected. This slice does not remove SQL
syntax, functions, data types, collations, supported storage engines,
diagnostics, or public C API behavior. Performance Schema remains outside the
core embedded profile.

## Database-Directory And Lifecycle Impact

None. Runtime directory layout, storage files, temporary files, and lock
behavior are unchanged.

## Public API Impact

None. `libmylite` headers and symbols are unchanged.

## Native Storage Impact

None. InnoDB, MyISAM, Aria, and MEMORY coverage should continue to link and
run against the same native engine members.

## Binary-Size Impact

The first step is archive-only: 712,680 bytes from debug/local-symbol
stripping. Disabling Performance Schema removes unused static plugin members
and brings the current archive to 31,529,704 bytes / 30.07 MiB, 1,599,936 bytes
smaller than the symbol-stripped baseline with Performance Schema still built.

## License Or Dependency Impact

No new dependencies or license changes. The wrapper uses standard `strip` and
`ranlib` tools already expected in the native build toolchain.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The embedded build wrapper produces a stripped `libmariadbd.a` by default.
- `STRIP_ARCHIVE=0` preserves an unstripped archive for diagnostics.
- Performance Schema is omitted from the embedded archive and remains omitted
  or disabled at runtime.
- The stripped archive still links `libmylite` and all embedded tests.
- The measured archive size and member count are recorded in the build
  documentation.
- Compatibility documentation remains unchanged because no runtime behavior is
  removed.

## Risks And Unresolved Questions

- Stripping local symbols reduces archive-level debugging and postmortem
  symbol inspection. Developers can rebuild with `STRIP_ARCHIVE=0` when that
  matters.
- Larger size wins require removing or stubbing code. Those changes need
  separate compatibility decisions before they are accepted.
