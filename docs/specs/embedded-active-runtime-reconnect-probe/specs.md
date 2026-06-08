# Embedded Active Runtime Reconnect Probe

## Problem

Production WordPress PHPUnit timing is dominated by PHP processes repeatedly
opening and closing MyLite-backed mysqli connections. The existing production
open-phase probe shows the expensive part is MariaDB embedded runtime startup
and shutdown, not the PHP wrapper, `mysql_real_connect()`, or system-table
bootstrap.

The current probe measures full warm open/close loops only. It does not show
how fast a second same-process `mylite_open()` can be when the global embedded
runtime is already active for the same database directory. That missing metric
makes future PHP adapter optimization ambiguous: a runtime-cache design may be
large and lifecycle-sensitive, but the potential win should be measurable
before changing adapter close semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:start_runtime()` increments
  `g_runtime.ref_count` and returns without calling `mysql_server_init()` when
  a matching same-process runtime is already active for the same database
  directory, access mode, ownerless mode, and durability policy.
- `packages/libmylite/src/database.cc:connect_runtime()` still creates a
  per-handle embedded `MYSQL` connection through `mysql_init()` and
  `mysql_real_connect()`.
- `packages/libmylite/src/database.cc:mylite_close()` closes the per-handle
  connection and calls `release_runtime()`.
- `packages/libmylite/src/database.cc:release_runtime()` skips
  `mysql_thread_end()` and `mysql_server_end()` while the runtime reference
  count remains above zero, and performs full MariaDB embedded shutdown only
  when the last handle closes.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens each mysqli
  link with `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE` and closes its
  `mylite_db *` when `mysqli_close()` runs or the PHP object is destroyed.
- `docs/api/libmylite-c-api.md` documents `mylite_close()` as the normal point
  where a handle releases runtime resources and final close removes transient
  runtime directories. Caching a runtime after close belongs in a separate
  explicit PHP-adapter design.
- A same-session production A/B on 2026-06-08 tested an embedded InnoDB generic
  thread-pool cap as a possible native startup/shutdown optimization. Max `8`,
  `16`, and `32` workers did not beat the default ordinary warm open/close
  path used by WordPress:
  - default pool: ordinary warm open/close `339.986 ms`, ownerless warm
    open/close `391.171 ms`;
  - cap `16`: ordinary warm open/close `363.639 ms`, ownerless warm open/close
    `368.583 ms`;
  - cap `32`: ordinary warm open/close `411.649 ms`, ownerless warm open/close
    `366.554 ms`.

## Scope And Non-Goals

In scope:

- Add ordinary and ownerless active-runtime reconnect timings to
  `mylite_embedded_performance_probe`.
- Reuse the existing open-phase counters so the new reconnect metrics expose
  whether `mysql_server_init()` and `mysql_server_end()` are skipped while an
  anchor handle remains open.
- Keep production CI and local timing jobs on production presets.
- Document why the native InnoDB thread-pool cap was rejected for the ordinary
  WordPress path.

Out of scope:

- Changing `libmylite` public close semantics.
- Adding a PHP mysqli runtime cache.
- Holding a hidden runtime after `mysqli_close()`.
- Changing InnoDB worker scheduling, redo, recovery, or file formats.
- Adding hard CI timing thresholds.

## Design

Extend `mylite_embedded_performance_probe` with active-runtime reconnect loops:

1. Open an anchor handle for the ordinary database mode.
2. Enable the existing embedded open-phase counters.
3. Repeatedly open and close a second handle to the same directory with the
   same ordinary flags.
4. Disable counters, emit
   `mylite_perf_ordinary_active_runtime_reconnect_*`, then close the anchor
   outside the measured interval.
5. Repeat the same flow for ownerless read/write flags.

The loop still uses real `mylite_open()` and `mylite_close()` calls. The only
difference from the full warm open/close metric is that another handle keeps
the process-global embedded runtime referenced, so `start_runtime()` and
`release_runtime()` should exercise the existing fast reference-count paths.

## Compatibility Impact

No SQL, PHP, mysqli, public C API, or native storage behavior changes. The
slice only adds performance-probe output.

## Database Directory And Lifecycle Impact

No directory-layout change. The measured reconnect loop uses the same database
directory as the existing probe and closes the anchor after collecting the
metrics.

## Native Storage Impact

No native storage format or recovery change. The InnoDB thread-pool cap was
measured and rejected for this slice.

## Build, Size, And Dependency Impact

No new dependencies. The production probe executable gains one small helper and
additional output keys; `libmylite` and public headers do not change.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced production probe and confirm ordinary and ownerless
  `active_runtime_reconnect` keys are emitted with open-phase counters.
- Run the default production probe and record the new metrics.
- Run the WordPress production `perf-probe` to keep PHP-facing timing evidence
  visible.
- Run focused embedded lifecycle and ownerless tests.
- Run `cmake --build --preset prod`, `ctest --preset prod`, production format
  and tidy checks, and `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`: passed.
- A reduced probe with `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2`,
  `MYLITE_PERF_SELECT_ITERATIONS=5`, and `MYLITE_PERF_INSERT_ITERATIONS=2`
  emitted the new active-runtime reconnect keys. Ordinary active-runtime
  reconnect averaged `4.448 ms`, ownerless active-runtime reconnect averaged
  `2.460 ms`, and both loops reported `0.000 ms` for
  `start_mysql_server_init` and `release_mysql_shutdown`.
- The default production probe reported ordinary cold create/open/close
  `649.980 ms`, ordinary warm open/close `335.243 ms`, ordinary active-runtime
  reconnect `1.934 ms`, ownerless warm open/close `396.215 ms`, ownerless
  active-runtime reconnect `2.197 ms`, ordinary direct `SELECT 1`
  `4582.98 ops/s`, ordinary prepared `SELECT 1` `2352.45 ops/s`, ordinary
  transactional inserts `2002.28 ops/s`, ordinary autocommit inserts
  `2081.63 ops/s`, ownerless direct `SELECT 1` `3917.00 ops/s`, ownerless
  prepared `SELECT 1` `1831.08 ops/s`, ownerless transactional inserts
  `963.40 ops/s`, and ownerless autocommit inserts `175.12 ops/s`.
- In the default active-runtime reconnect loops, ordinary
  `start_mysql_server_init` and `release_mysql_shutdown` averaged `0.000 ms`;
  ownerless `start_mysql_server_init` and `release_mysql_shutdown` also
  averaged `0.000 ms`. The remaining ordinary reconnect cost was connection
  creation at `0.355 ms` average, system-table statements at `1.042 ms`
  average, rollback at `0.220 ms` average, and connection close at `0.220 ms`
  average.
- `MYLITE_WORDPRESS_PHASE=perf-probe` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod` and
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` passed. It reported stock PHP
  process startup `53.953 ms`, PHP-with-MyLite-extension startup
  `173.350 ms`, process plus connect/close `567.649 ms`, process/connect
  delta `394.299 ms`, in-process mysqli connect/close `398.453 ms`,
  `SELECT 1` `280.55 ops/s`, transactional inserts `411.19 ops/s`, point
  selects `228.93 ops/s`, prepared autocommit inserts `377.73 ops/s`, direct
  autocommit inserts `613.41 ops/s`, and `wordpress_total_seconds=55`.
- Focused embedded CTest selectors passed:
  `libmylite.embedded-ownerless-innodb-lock-hooks`,
  `libmylite.ownerless-primitives`, `libmylite.embedded-open-close`,
  `libmylite.embedded-storage`, and
  `libmylite.embedded-transactions-recovery`.
- `cmake --build --preset prod`: passed.
- `ctest --preset prod --output-on-failure`: passed `24/24`.
- `cmake --build --preset format-check-prod`: passed.
- `cmake --build --preset tidy-prod`: passed.
- `git diff --check`: passed.

## Acceptance Criteria

- Existing full warm open/close keys remain unchanged.
- The probe emits ordinary and ownerless active-runtime reconnect averages.
- The active-runtime reconnect open-phase output shows reconnect loops are not
  paying full MariaDB embedded startup and shutdown while the anchor is open.
- No native scheduling cap or hidden runtime-cache behavior is committed.

## Risks And Follow-Up

- Active-runtime reconnect timings are a diagnostic upper bound, not a product
  behavior change. A PHP adapter cache would need explicit design because the
  WordPress process-isolated harness closes parent `wpdb` handles before
  spawning children to release directory locks.
- Timing remains host-sensitive. Use production same-session comparisons when
  deciding whether to optimize a phase.
