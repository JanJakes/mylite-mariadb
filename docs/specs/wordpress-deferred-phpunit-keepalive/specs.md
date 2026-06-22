# WordPress Deferred PHPUnit Keepalive

## Problem

The WordPress PHPUnit matrix already keeps the embedded runtime alive for
non-isolated shards with `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`, but the
deferred-reconnect shards still run with keepalive disabled. Those shards also
set `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`, so simply flipping the
matrix bit would not have helped: the PHPUnit process wrapper inserted the
keepalive reopen inside the same reconnect guard that skips reconnecting
WordPress `$wpdb` objects after each child process.

That shape leaves deferred child processes exposed to repeated full embedded
MariaDB lifecycle cost. Current production probe evidence keeps active-runtime
reconnect below 1 ms, while warm open/close remains roughly 100 ms, so keeping
the parent runtime warm between isolated children is the high-impact path.

## Source References

- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit
  `DefaultPhpProcess.php` so parent WordPress database handles are closed
  before a process-isolated child starts.
- The same patch creates a harness-owned mysqli keepalive when
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`, closes it before child execution,
  and reopens it after the child.
- `.github/workflows/ci.yml` defines the deferred shards with
  `reconnect_after_child: "0"` to avoid reconnecting application-visible
  `$wpdb` handles after process-isolated child execution.

## Design

Keep `$wpdb` reconnect policy unchanged. The deferred shards still set
`MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`, so application-visible
WordPress handles are not reconnected after child process execution.

Move the keepalive reopen outside that reconnect guard. The wrapper still
closes the keepalive before the child runs, so single-directory locking and
baseline restore remain safe. After the child exits, the wrapper reopens only
the harness-owned keepalive when `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`.

Enable keepalive for:

- `deferred-reconnect-baseline-restored`
- `deferred-reconnect-skip-install`
- `deferred-reconnect-ui`

The database shard remains keepalive-off because it still runs an explicit
profile pass and should keep measuring process-style behavior. Non-isolated
shards keep their existing keepalive behavior.

## Compatibility

This is a CI harness performance change only. It does not change SQL behavior,
mysqli public API behavior, MyLite storage files, ownerless page-version WAL,
native redo, or recovery policy.

For baseline-restored children, the wrapper closes the keepalive before the
child baseline restore copies the prepared database tree, then reopens the
keepalive after the child exits. The baseline restore ordering remains
conservative with respect to native file ownership.

## Verification

- Run `bash -n` on the changed shell tools.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'`.
- Inspect the production performance probe evidence that active runtime
  reconnect remains below full warm open/close cost.

## Acceptance Criteria

- The PHPUnit wrapper contains an explicit keepalive reopen marker outside the
  `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD` guard.
- The three deferred WordPress PHPUnit shard matrix entries keep
  `reconnect_after_child: "0"` and set `keepalive: "1"`.
- The CI production-build audit fails if those shard settings or the wrapper
  marker are removed.
