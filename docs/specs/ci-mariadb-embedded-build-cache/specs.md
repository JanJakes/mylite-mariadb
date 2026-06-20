# CI MariaDB Embedded Build Cache

## Problem

The ownerless branch now runs CI timing jobs with production builds, and the
current green `ownerless-concurrency` head shows the longest repeated setup
cost is rebuilding MariaDB's embedded archive in separate jobs. The
`ubuntu-embedded` job spent about 5.5 minutes in `Build MariaDB embedded
archive`, while the WordPress setup job spent almost 7 minutes in `Build MyLite
PHP extensions for WordPress PHPUnit`, mostly inside the separate WordPress
MariaDB embedded archive build.

Those builds are production `MinSizeRel` archive builds, so removing them from
timing would make the evidence less useful. Reusing exact matching build
directories across CI runs keeps the production build shape while reducing
repeat-run wall time after the cache is populated.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` builds the normal embedded archive in
  `build/mariadb-embedded`.
- The WordPress harness builds its archive in
  `build/wordpress-mariadb-embedded`, mounted into Docker as
  `/work/build/wordpress-mariadb-embedded`.
- `tools/mariadb-embedded-build ensure` configures only when the build cache is
  missing, when the absolute build directory does not match the CMake cache, or
  when the embedded baseline profile content no longer matches the profile
  signature marker written by the last successful configure.
- `tools/require-cmake-build-type MinSizeRel ...` already guards both embedded
  archive build directories before downstream production timings are reported.
- A warm-cache rerun of the first cache slice restored both archive caches, but
  still reconfigured. GitHub checkout gave
  `cmake/mariadb-embedded-baseline.cmake` a newer mtime than the restored
  `CMakeCache.txt`, so the old `profile -nt CMakeCache.txt` freshness check
  defeated a valid exact-key cache hit.

## Design

Add exact-key GitHub Actions caches for the two MariaDB embedded archive build
directories:

- `build/mariadb-embedded`;
- `build/wordpress-mariadb-embedded`.

The keys hash the MariaDB source tree, the embedded baseline profile, and the
embedded build wrapper. The WordPress cache key also hashes the WordPress
PHPUnit harness because that script owns the Docker build environment and
container path mapping used for the archive.

Do not use prefix `restore-keys` for the archive caches. A cache miss should
fall back to a normal production build instead of restoring a build tree from a
different MariaDB source or baseline profile. After restore or rebuild, the
existing production build-type guards still verify `MinSizeRel`.

Switch the top-level embedded CI job from `tools/mariadb-embedded-build all` to
`tools/mariadb-embedded-build ensure`. Cold cache behavior still configures,
builds, strips, and measures the archive. Warm cache behavior preserves the
existing CMake cache and lets Ninja decide whether `libmariadbd.a` has work.

Teach `tools/mariadb-embedded-build` to write
`.mylite-embedded-profile.sha256` inside the build directory after a successful
configure. `ensure` compares the current profile content hash with that marker
instead of comparing checkout/cache mtimes. Missing markers, profile content
changes, explicit CMake arguments, missing CMake caches, or absolute build
directory mismatches still force configure.

Update `tools/check-ci-production-builds` so workflow edits that remove these
caches, cache paths, exact keys, or the `ensure` entry point fail the local and
CI production-build audit.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, native storage, ownerless locking, or
directory-lifecycle behavior changes.

## Directory And Lifecycle Impact

No durable database-directory changes. CI may restore build artifacts under
`build/mariadb-embedded` and `build/wordpress-mariadb-embedded`; runtime test
database directories remain unchanged.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage format changes. The embedded archive is still built from the
same MariaDB source and `cmake/mariadb-embedded-baseline.cmake` profile.

## Build And Performance Impact

The first CI run after a new source/profile hash still pays the full production
MariaDB embedded archive build. Subsequent runs with the same hash can restore
the build directory and should spend only cache restore time plus any no-op
Ninja/measure work. The optimization targets CI setup wall time and does not
claim an engine-runtime improvement.

The cache keys intentionally prefer correctness over broad reuse. Runner image
or Docker base-image changes may still reuse a cache with the same source hash;
the production build-type guards catch configuration drift, and exact source
keys avoid restoring stale MariaDB outputs across code changes.

## Test Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/mariadb-embedded-build`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest audit selector for CI production builds.
- Run `tools/mariadb-embedded-build ensure` against the local warmed
  `build/mariadb-embedded` tree and confirm configure is skipped.
- Run `tools/mariadb-embedded-build ensure` twice after adding the profile
  signature marker and confirm the second run skips configure.
- Verify the local WordPress MariaDB embedded cache remains `MinSizeRel`.
- Run `git diff --check`.

## Acceptance Criteria

- The embedded CI job restores `build/mariadb-embedded` before running
  `tools/mariadb-embedded-build ensure`.
- The WordPress setup job restores `build/wordpress-mariadb-embedded` before
  the harness build phase.
- Neither MariaDB archive cache uses prefix restore keys.
- The CI production-build audit requires both cache steps, both cache paths,
  both exact keys, and the `ensure` command.
- A restored cache is invalidated by profile content drift, not by checkout
  mtime drift.
- Production build-type guards remain in place after the restore/build phase.

## Verification Results

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/mariadb-embedded-build`: passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed, 1/1 test.
- `tools/mariadb-embedded-build ensure`: passed on the warmed
  `build/mariadb-embedded` tree with `mariadb_embedded_configure=skipped`,
  `ninja: no work to do.`, `CMAKE_BUILD_TYPE=MinSizeRel`, and a 36.87 MiB
  `libmariadbd.a`.
- Warm-cache CI rerun before the profile-signature fix restored both archive
  caches, but `ubuntu-embedded` still spent about 5m47 in `Build MariaDB
  embedded archive`; this confirmed checkout/cache mtime drift was forcing
  configure on valid exact-key cache hits.
- After adding `.mylite-embedded-profile.sha256`,
  `tools/mariadb-embedded-build ensure` configured once on the local build tree
  with no existing marker, wrote the marker, and completed with a no-op Ninja
  build.
- A second `tools/mariadb-embedded-build ensure`: passed with
  `mariadb_embedded_configure=skipped`.
- `touch cmake/mariadb-embedded-baseline.cmake` followed by
  `tools/mariadb-embedded-build ensure`: passed with
  `mariadb_embedded_configure=skipped`, proving profile mtime drift alone no
  longer invalidates the cache.
- `tools/require-cmake-build-type MinSizeRel
  build/wordpress-mariadb-embedded`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed after the profile-signature fix, 1/1 test.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.
