# WordPress PHPUnit Static Scan Type Filter

## Goal

Keep the WordPress process-isolated PHPUnit parent cleanup path cheap without
weakening the required MyLite database lock release before each child
`proc_open()`.

The parent process must close live MyLite-backed `wpdb` handles before PHPUnit
launches an isolated child process. The existing harness already caches
reflection discovery, but it still records every static property in declared
classes. Typed static properties that can only hold scalar, array, or other
non-object values cannot be a `wpdb`-like handle and do not need to be checked
before each child launch.

## Design

`tools/wordpress-phpunit-mysqli-mylite` now injects
`MYLITE_WORDPRESS_STATIC_WPDB_TYPE_FILTER` into PHPUnit's
`DefaultPhpProcess.php` patch. During static-property cache construction, it
keeps properties whose declared type can hold an object:

- untyped properties,
- non-builtin class/interface/object-like types,
- `mixed`,
- `object`,
- union types containing one of those object-capable alternatives.

It skips typed properties that cannot hold an object. The scan still reads the
current value of every retained property before each child process, so a
retained static property that changes to a `wpdb`-like handle is still closed.

The child-process profile now also reports, for clean patched PHPUnit vendor
trees:

- `wordpress_phpunit_child_process_static_properties`,
- `wordpress_phpunit_child_process_skipped_static_properties`.

A later profile-output follow-up also derives per-child average timing keys
from the existing totals:

- `wordpress_phpunit_child_process_lock_release_ms_avg`,
- `wordpress_phpunit_child_process_runtime_ms_avg`,
- `wordpress_phpunit_child_process_reconnect_ms_avg`.

The dependency patcher can upgrade an existing cached MyLite-patched
`DefaultPhpProcess.php` that has the older reflection cache marker but not the
new type-filter marker.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or storage behavior changes. This is
a WordPress PHPUnit harness optimization only. It does not remove the close and
reconnect sequence that prevents parent-held MyLite directory locks from
blocking process-isolated children.

## Performance Impact

This is a narrow cleanup, not the main WordPress PHPUnit bottleneck. Fresh
Release measurements on 2026-06-08 showed the current ownerless branch is not
slower than main for the focused database suite:

- branch `^Tests_DB`: PHPUnit `14.088s`, shell real `26.040s`;
- main `4760d512` with a Release PHP extension build: PHPUnit `19.842s`,
  shell real `33.629s`.

CI-sized mysqli perf probes with Release PHP extension builds showed the branch
and main are close on ordinary non-ownerless mysqli reads and prepared writes:

- branch process plus connect/close `555.440 ms`, in-process connect/close
  `399.288 ms`, active-runtime reconnect `3.367 ms`, `SELECT 1`
  `288.20 ops/s`, prepared autocommit inserts `402.36 ops/s`, direct
  autocommit inserts `753.87 ops/s`;
- main process plus connect/close `481.876 ms`, in-process connect/close
  `358.703 ms`, active-runtime reconnect `5.716 ms`, `SELECT 1`
  `289.99 ops/s`, prepared autocommit inserts `384.88 ops/s`, direct
  autocommit inserts `272.91 ops/s`.

The embedded open-phase probe shows ordinary ownerless coordination setup is
not the process-start bottleneck: ordinary warm open/close averaged
`372.983 ms`, with `mysql_server_init()` about `129.503 ms` per open and
`mysql_server_end()` about `240.158 ms` per close. Ordinary coordination
metadata, shared-memory prepare/map, page-log open, and checkpoint open were
all sub-millisecond per open.

## Verification

- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- production `MYLITE_WORDPRESS_PHASE=dependencies` upgraded the warmed
  `build/wordpress-phpunit-tools` vendor patch cleanly.
- a clean `build/wordpress-phpunit-tools-typefilter-clean` dependency phase
  produced a fresh patched `DefaultPhpProcess.php` containing
  `MYLITE_WORDPRESS_STATIC_WPDB_TYPE_FILTER` and the new profile keys.
- focused production `Tests_Formatting_Emoji` against the upgraded warmed
  vendor tree passed 19 tests with 4 child processes, lock release `1.031808s`,
  child runtime `18.963796s`, reconnect `0.491505s`, shell real `34.366s`.
- focused production `Tests_Formatting_Emoji` against the clean patched vendor
  tree passed 19 tests with 4 child processes, `4531` retained static
  properties, `10` skipped typed static properties, lock release `1.050638s`,
  child runtime `15.810013s`, reconnect `0.461563s`, shell real `28.949s`.
- branch Release `^Tests_DB` passed 651 tests with 3 skips, PHPUnit `14.088s`,
  shell real `26.040s`.
- main `4760d512` Release PHP extension comparison for `^Tests_DB` passed 651
  tests with 3 skips, PHPUnit `19.842s`, shell real `33.629s`.

## Remaining Performance Work

- Process-isolated WordPress tests still pay repeated child-process PHP startup
  and MyLite/MariaDB embedded open/close cost; the profile now prints
  per-child averages so that cost is visible without manual division.
- The biggest open/close target remains MariaDB embedded `mysql_server_init()`
  and `mysql_server_end()`, not ordinary ownerless coordination metadata.
- Hidden runtime caching after `mysqli_close()` would be a compatibility change
  and needs a separate design.
