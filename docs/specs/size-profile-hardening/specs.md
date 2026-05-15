# Size Profile Hardening

## Goal

Make size-profile work measurable across both the MariaDB embedded archive and
linked MyLite runtime artifacts before applying source-level trimming. The first
slice adds a reproducible size report that future pruning commits can compare
against.

## Non-Goals

- Do not remove charsets, collations, SQL functions, storage engines, or parser
  sources in this slice.
- Do not change the default embedded CMake profile.
- Do not claim a production package size floor from test executables alone.
- Do not apply historical `bundle-size` branch changes without rerunning them on
  the current branch.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48642ed6d21e54668641d5f31475f62fa0e`.
- `mariadb/libmysqld/CMakeLists.txt` builds the Unix embedded static archive as
  `libmariadbd.a` by merging `sql_embedded`, embedded plugin libraries, and
  shared MariaDB support libraries into the `mysqlserver` target.
- `cmake/MyLiteMariaDB.cmake` imports that archive as
  `MyLite::MariaDBEmbedded` and links first-party `libmylite` targets to the
  host OpenSSL, PCRE2, threads, zlib, `dl`, and `m` dependencies required by the
  archive.
- `packages/libmylite/CMakeLists.txt` provides the linked artifacts that best
  represent current MyLite embedded runtime reachability:
  `mylite_embedded_open_close_test`, `mylite_embedded_exec_test`, and the
  storage-smoke engine test.
- Upstream MariaDB still sets `ENABLE_EXPORTS TRUE` on several client, server,
  and embedded example executables in `mariadb/client/CMakeLists.txt`,
  `mariadb/sql/CMakeLists.txt`, and `mariadb/libmysqld/examples/CMakeLists.txt`.
  MyLite-owned smoke executables do not currently set `ENABLE_EXPORTS`, so the
  historical ranked item about removing exports from MyLite-owned smokes is not
  directly applicable to this branch.
- `tools/mariadb-embedded-build measure` records archive size and member count
  only. Historical size research shows linked artifacts can move independently
  from the static archive, so archive-only reporting is insufficient for
  accepting or rejecting future hardening changes.

## Compatibility Impact

This slice is measurement-only. It does not change SQL behavior, the public
`libmylite` C API, storage-engine routing, metadata routing, wire protocol, or
unsupported server surfaces. `docs/COMPATIBILITY.md` does not need an update for
this slice.

## Design

Add a first-party `tools/mylite-size-report` script that writes a Markdown
report for the current local build outputs:

- baseline MariaDB embedded archive,
- storage-smoke MariaDB embedded archive,
- embedded open/close and direct-exec linked tests,
- storage-smoke open/close, direct-exec, and storage-engine linked tests.

The report records unstripped bytes for every present artifact. For linked
artifacts it also records the size of a stripped temporary copy and a count of
defined global symbols. For archives it records the member count. Missing
artifacts are reported as missing instead of hiding partial build state.

Keep the existing `tools/mariadb-embedded-build measure` command intact for the
archive-focused baseline workflow.

## File Lifecycle

The tool reads build artifacts and writes `build/mylite-size-report.md` by
default. It does not open `.mylite` databases, create durable companions, or
change runtime file lifecycle.

## Embedded Lifecycle And API

No embedded runtime or public API behavior changes. The linked artifacts are
used as size proxies only; they are not new supported user-facing binaries.

## Build, Size, And Dependencies

The tool uses standard local shell utilities already required or commonly
available for the build: `stat`, `awk`, `ar`, `strip`, and `nm`. `strip` and
`nm` are optional for report fields; missing tools produce `n/a` fields rather
than changing artifacts in place. The report strips only temporary copies.

## Test Plan

1. Build the existing dev, embedded-dev, and storage-smoke-dev presets.
2. Run `tools/mylite-size-report` and inspect the generated Markdown report.
3. Run the existing embedded and storage-smoke tests to prove reporting did not
   change runtime behavior.
4. Run format, tidy, diff, and archive-size checks used by nearby roadmap
   slices.

## Acceptance Criteria

- A size report can be generated from current local build outputs.
- The report includes both static archive and linked-runtime size signals.
- The roadmap and architecture docs point future size-profile work at the
  report instead of archive-only evidence.
- No source trimming is accepted without fresh report evidence.

## Risks And Open Questions

- Test executables remain imperfect package proxies. A later packaging slice
  should add extension-shaped shared-library probes once the desired runtime
  integration targets are concrete.
- Platform `strip` behavior differs between macOS and Linux, so stripped sizes
  are local comparison evidence, not absolute cross-platform release numbers.
- The highest-ranked historical wins involve compatibility tradeoffs such as
  charset and collation removal; those need separate specs and comparison
  coverage before implementation.
