# WordPress Process Close Attribution

## Problem

The WordPress PHPUnit job now exposes production build, setup, perf-probe, and
test-only timings separately, but the process-isolated test classes still pay a
large per-child MyLite lifecycle cost. The current production probes already
show that active-runtime reconnect is cheap while full process connect/close is
expensive. The remaining ambiguity is whether the child process path is paying a
material extra cost because the probe uses an explicit `mysqli_close()` while
real process-isolated children usually leave the connection object to PHP
object cleanup at request shutdown.

Skipping `mylite_close()` from PHP object cleanup would not be a safe
optimization without a separate durability and lifecycle API: the extension's
object destructor currently clears the statement cache and performs a real
`mylite_close()`, and explicit `mysqli_close()` does the same close path. The
next useful performance slice is therefore attribution, not a semantic shortcut.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens mysqli links
  through `mylite_open(path, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, ...)`.
  The WordPress adapter therefore exercises the ordinary embedded runtime, not
  ownerless read/write opens.
- `MyLite_MySQLi::close()` and global `mylite_mysqli_close()` both clear the
  query cache and call `php_mylite_mysqli_profiled_close(...,
  PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT)`.
- `php_mylite_mysqli_link_free()` clears the query cache, clears the recent
  result SQL, and calls `php_mylite_mysqli_profiled_close(...,
  PHP_MYLITE_MYSQLI_PROFILE_CLOSE_OBJECT_FREE)` when a live link remains.
- `php_mylite_mysqli_profiled_close()` always calls `mylite_close()`, so both
  explicit and object-free paths pay the same supported MyLite close semantics.
- `tools/wordpress-phpunit-mysqli-mylite` already has a `perf-probe` phase that
  reports stock PHP startup, MyLite-extension PHP startup, process plus
  explicit connect/close, in-process connect/close, active-runtime reconnect,
  and steady SQL rates under CI's production build guards.

## Design

Extend the WordPress `perf-probe` phase with a second bounded process shape that
connects to the prepared MyLite database and then exits without calling
`mysqli_close()`. PHP object cleanup still closes the link through
`php_mylite_mysqli_link_free()`, but the new measurement matches the
implicit-close shape used by process-isolated PHPUnit children.

Measure the explicit-close and implicit object-free shapes in alternating order
and average each shape separately. That avoids a misleading result where the
second shape in a serial loop gets all of the warmed database-directory and
host-cache effects.

Keep the existing `wordpress_perf_php_connect_process_ms_avg` and summary keys
as the explicit-close measurement for compatibility with existing log readers.
Add explicit names for both paths:

- `wordpress_perf_php_connect_process_explicit_close_ms_avg`
- `wordpress_perf_php_connect_process_implicit_object_free_ms_avg`
- `wordpress_perf_php_connect_process_implicit_object_free_delta_ms_avg`
- `wordpress_perf_php_connect_process_implicit_minus_explicit_ms_avg`
- `wordpress_perf_php_connect_process_close_shape_order=alternating`

The same aliases are emitted under `wordpress_perf_summary_*` so CI summaries
remain grep-friendly.

## Compatibility Impact

No SQL, PHP API, mysqli API, MyLite runtime, or database-directory behavior
changes. The added script exits normally and relies on the existing PHP object
destructor to close the live mysqli link.

## Directory And Lifecycle Impact

No lifecycle semantics change. The slice explicitly avoids treating process exit
as permission to skip `mylite_close()`.

## Build And Performance Impact

The CI WordPress perf probe runs one additional process loop using the existing
`MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS` bound. CI currently sets that bound
to five iterations, so the extra wall time is small relative to the full
WordPress job and produces direct evidence for the per-child close path.

A reduced production-shaped verification run using the CI-pinned WordPress ref,
`Release` MyLite build, `MinSizeRel` MariaDB embedded build, five process and
connect iterations, and tiny SQL/write loops reported:

- explicit process connect/close: `759.288 ms`;
- implicit object-free process connect/close: `617.743 ms`;
- implicit minus explicit: `-141.545 ms`;
- active-runtime reconnect: `3.644 ms`.

The implicit process-shutdown shape was not slower than explicit close in this
sample, so the remaining process-isolated PHPUnit cost stays attributed to full
embedded lifecycle startup/shutdown rather than a PHP destructor-specific tax.

## Test Strategy

- Shell syntax check for `tools/wordpress-phpunit-mysqli-mylite`.
- `tools/check-ci-production-builds` to keep production timing guards intact.
- A reduced production `MYLITE_WORDPRESS_PHASE=perf-probe` run with five
  process/connect iterations to verify the new keys are emitted by the real
  wrapper and PHP extension.
