# TC Log Mmap Trim

## Problem Statement

The no-binlog embedded profile still links MariaDB's mmap-backed transaction
coordinator log. That implementation owns the inherited `tc.log` file used for
two-phase and XA recovery when the binary log is not the transaction
coordinator. MyLite's current core profile supports ordinary local
transactions through MariaDB native engines, but it does not claim external XA
or durable multi-resource two-phase recovery.

Removing this path is a safe size step only if the unsupported boundary is
explicit. The slice therefore rejects top-level `XA` SQL through the public
policy layer and omits the mmap `tc.log` implementation from the no-binlog
embedded archive.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/log.h` defines `TC_LOG`, `TC_LOG_DUMMY`, and `TC_LOG_MMAP`.
  The real `TC_LOG_MMAP` class exists when `HAVE_MMAP` is set.
- `mariadb/sql/log.cc` implements `TC_LOG_MMAP::open()`, `close()`,
  `recover()`, `log_and_order()`, `unlog()`, and checkpoint cleanup. The
  implementation creates or opens `opt_tc_log_file`, mmaps it, records
  transaction XIDs, runs crash recovery through `ha_recover()`, and deletes the
  file on clean close.
- `mariadb/sql/mysqld.cc` defaults `opt_tc_log_file` to `tc.log`, opens
  `tc_log` during startup, and publishes `Tc_log_*` status variables.
- `mariadb/sql/handler.cc` uses the selected `tc_log` in commit, XA prepare,
  and checkpoint notification paths.
- MariaDB's help text describes external `XA` statements as distributed
  transaction support with an external transaction manager. That is outside
  MyLite's current local embedded API contract.

## Design

Add `MYLITE_WITH_TC_LOG_MMAP`, defaulting `ON` for inherited MariaDB embedded
builds and forced `OFF` by the MyLite embedded baseline. The disabled option
requires `MYLITE_WITH_BINLOG_CORE=OFF`, because the trim is valid only for the
no-binlog profile.

When disabled:

- `TC_LOG_MMAP` aliases to `TC_LOG_DUMMY`;
- `get_tc_log_implementation()` returns `tc_log_dummy` in the no-binlog
  embedded profile;
- the real `TC_LOG_MMAP` methods and vtable are not compiled;
- inert `opt_tc_log_size` and `Tc_log_*` status globals remain available for
  inherited status/sysvar references; and
- `libmylite` rejects top-level `XA` statements before MariaDB dispatch.

## Compatibility Impact

Ordinary `START TRANSACTION`, `COMMIT`, `ROLLBACK`, savepoints, clean reopen,
and covered child-process crash recovery remain supported through native
storage engines. External XA statements such as `XA START`, `XA PREPARE`,
`XA COMMIT`, `XA ROLLBACK`, and `XA RECOVER` become explicitly unsupported in
the default embedded core.

This slice does not remove the generic transaction coordinator call sites or
claim final MyLite recovery semantics for multiple XA-capable resources.

## Database-Directory Impact

The default embedded profile no longer contains the executable mmap code that
creates, recovers, or deletes a `tc.log` sidecar. Tests assert that `tc.log` is
not created in the MyLite database directory during supported server-surface
coverage.

## Binary-Size Impact

Before implementation, the measured stripped embedded archive was:

| Artifact | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,047,312 | 24.84 | 693 |

After omitting the mmap-backed transaction coordinator, the measured stripped
archive is:

| Artifact | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,039,248 | 24.83 | 693 |

The slice saves 8,120 pre-strip bytes and 8,064 stripped bytes with no archive
member-count change.

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

Additional checks:

- the embedded archive reports `MYLITE_WITH_TC_LOG_MMAP:BOOL=OFF`;
- the embedded archive no longer defines real `TC_LOG_MMAP` methods or vtable;
- direct and prepared `XA` SQL fail with the stable server-surface diagnostic;
  and
- `tc.log` is absent from the supported server-surface lifecycle.

## Acceptance Criteria

- The embedded archive builds and measures with `MYLITE_WITH_TC_LOG_MMAP=OFF`.
- Existing transaction and recovery tests still pass.
- Server-surface policy tests cover direct and prepared `XA` rejection.
- No supported lifecycle creates `tc.log`.
- Build, test, format, tidy, and diff checks pass.

## Risks And Unresolved Questions

- External XA and durable multi-resource two-phase recovery are not supported in
  the default embedded profile. A future slice must design MyLite-owned
  recovery before making those claims.
- `Tc_log_*` status variables remain as inherited inert counters unless a later
  status-variable trim removes them from the default profile.
