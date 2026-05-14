# Minimal Embedded Build

## Goal

Create a reproducible local build profile for the MariaDB embedded library that
MyLite can use as its initial upstream baseline. The profile should build from
the imported MariaDB source without local MariaDB source edits and record enough
size and configuration data to make later trimming measurable.

Native storage configuration is not part of this baseline. This slice only
proves that the selected MariaDB embedded source can be configured, built, and
measured before later slices constrain engine files to the MyLite database
directory.

## Non-Goals

- Do not trim MariaDB beyond the options required to configure and build the
  imported source without fetching optional submodules.
- Do not implement `libmylite` open/close behavior.
- Do not configure native storage directory lifecycle or SQL smoke tests.
- Do not claim a production-supported component set from this baseline.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/CMakeLists.txt` enables the embedded server build through
  `WITH_EMBEDDED_SERVER` and adds `libmysqld`, examples, and embedded unit tests
  when enabled.
- `mariadb/libmysqld/CMakeLists.txt` builds the embedded library from
  `SQL_EMBEDDED_SOURCES`, `sql_embedded`, embedded plugin libraries, and the
  normal SQL server sources. On Unix it names the static output
  `libmariadbd.a`.
- `mariadb/cmake/submodules.cmake` allows `UPDATE_SUBMODULES=OFF`, but still
  requires `libmariadb/CMakeLists.txt`. The initial import includes that
  submodule and intentionally omits optional payloads.
- `mariadb/cmake/wsrep.cmake` defaults `WITH_WSREP` to `ON` on Unix and fails
  without the omitted `wsrep-lib` submodule, so this baseline must disable
  WSREP explicitly.
- `mariadb/storage/maria/CMakeLists.txt` wires S3 support through the omitted
  `storage/maria/libmarias3` submodule, so this baseline must disable the S3
  plugin explicitly.

## Architecture

The baseline profile should live outside `mariadb/` as MyLite-owned build
orchestration. MariaDB source edits are not required for this slice. The initial
profile should be narrow enough to work with the imported source tree, but broad
enough to preserve MariaDB behavior as the compatibility baseline:

- `UPDATE_SUBMODULES=OFF`
- `WITH_EMBEDDED_SERVER=ON`
- `WITH_UNIT_TESTS=OFF`
- `WITH_SSL=system`
- `WITH_ZLIB=bundled`
- `WITH_WSREP=OFF`
- `PLUGIN_S3=NO`

The profile should build the static embedded archive first. Shared-library
policy, symbol visibility, link mode, and downstream `libmylite` wrapping belong
to later slices after we have a measured baseline.

## Compatibility Impact

This slice does not change SQL behavior, public API behavior, file lifecycle, or
native storage behavior. It only records a buildable embedded baseline. The
explicitly disabled surfaces are optional distributed or remote-storage features
that depend on omitted submodule payloads and are outside the initial embedded
single-directory profile:

- WSREP/Galera replication
- Aria S3 helper/plugin support

Any later decision to disable more MariaDB components must be documented as a
compatibility decision with size evidence.

## Build And Size Evidence

The baseline should record:

- Host platform and toolchain versions used for the measurement.
- CMake generator, build type, and cache options.
- Embedded archive path and byte size.
- Embedded archive member count.
- Notable enabled and disabled MariaDB components.
- Required host dependencies discovered during configure or build.

On macOS, MariaDB's generated sources require a modern Bison. The build wrapper
may prefer Homebrew Bison when installed, but should keep the actual CMake cache
options explicit and reproducible. The baseline uses bundled zlib because
MariaDB's system-zlib path can add the macOS SDK root include directory as a
normal `-I` path, which can break libc++ header ordering on current macOS
toolchains.

## Test Plan

1. Configure MariaDB from the imported source tree with the committed baseline
   profile.
2. Build the embedded static archive target.
3. Measure the resulting archive size and member count.
4. Record the cache options and dependency notes in architecture documentation.

## Acceptance Criteria

- A committed MyLite-owned build profile configures MariaDB without fetching
  additional submodules.
- The embedded static archive target builds locally.
- Baseline size and configuration evidence is documented.
- The roadmap reflects that the minimal embedded build baseline is established.

## Risks And Open Questions

- The first measured embedded archive is still broad; trimming requires later
  component-level size evidence instead of changing this baseline by intuition.
- Host dependencies can still affect reproducibility, especially generated
  parser tooling on macOS.
- Further MariaDB options may be removable, but each removal needs a documented
  compatibility decision.
