# InnoDB Buffer-Pool Dump/Load Policy

## Problem

MyLite starts MariaDB with InnoDB buffer-pool dump/load disabled because the
advisory `ib_buffer_pool` file is not durable transaction state and is unsafe as
a shared optimization file under concurrent embedded processes. The startup
flags are not enough if SQL can later set dynamic `innodb_buffer_pool_*`
variables that trigger a dump/load or re-enable dump/load at shutdown/startup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/srv0srv.h` defines
  `SRV_BUF_DUMP_FILENAME_DEFAULT` as `ib_buffer_pool`.
- `mariadb/storage/innobase/srv/srv0srv.cc` defaults
  `srv_buffer_pool_dump_at_shutdown` and
  `srv_buffer_pool_load_at_startup` to true.
- `mariadb/storage/innobase/handler/ha_innodb.cc` exposes dynamic system
  variables for `innodb_buffer_pool_dump_now`,
  `innodb_buffer_pool_dump_at_shutdown`, `innodb_buffer_pool_dump_pct`,
  `innodb_buffer_pool_load_now`, `innodb_buffer_pool_load_abort`,
  `innodb_buffer_pool_load_pages_abort`, and
  `innodb_buffer_pool_load_at_startup`.
- `mariadb/storage/innobase/buf/buf0dump.cc` implements dump/load status and
  file IO for the buffer-pool dump/load path.
- `packages/libmylite/src/database.cc` already passes
  `--innodb-buffer-pool-dump-at-shutdown=OFF` and
  `--innodb-buffer-pool-load-at-startup=OFF` at embedded startup.

## Design

Treat InnoDB buffer-pool dump/load controls as unsupported server-owned SQL
surface. Reject system-variable assignments before MariaDB dispatch for:

- `innodb_buffer_pool_dump_now`
- `innodb_buffer_pool_dump_at_shutdown`
- `innodb_buffer_pool_dump_pct`
- `innodb_buffer_pool_load_now`
- `innodb_buffer_pool_load_abort`
- `innodb_buffer_pool_load_at_startup`
- `innodb_buffer_pool_load_pages_abort`

User variables and string literals with the same words remain valid.

## Scope And Non-Goals

In scope:

- Runtime SQL policy enforcement for InnoDB buffer-pool dump/load controls.
- Embedded server-surface policy tests.
- Documentation that startup disablement is now backed by SQL policy.

Out of scope:

- Removing the InnoDB dump/load code from the MariaDB baseline.
- Supporting `ib_buffer_pool` as a MyLite-owned durable file.
- Changing InnoDB native transaction recovery or page-version WAL behavior.

## Compatibility Impact

MariaDB servers allow these operational variables. MyLite rejects them because
the core library is an embedded single-directory runtime, not a daemon-managed
server with one process owning advisory optimization files. This is a deliberate
compatibility limitation; it does not affect normal SQL, native InnoDB durable
state, or transaction recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The policy prevents SQL from creating,
loading, or re-enabling the advisory `ib_buffer_pool` file path during the
embedded runtime lifecycle.

## Test Plan

- Build `mylite_embedded_server_surface_policy_test` in `embedded-dev`.
- Run the focused embedded server-surface policy test.
- Run embedded and hook ownerless SQL, ownerless stress, `format-check`, and
  diff checks.

## Acceptance Criteria

- SQL assignments to buffer-pool dump/load system variables fail with a MyLite
  policy error before MariaDB dispatch.
- User variables and string literals using the same names still work.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This enforces the documented disabled policy but does not remove the linked
  upstream InnoDB code. Size-oriented trimming remains separate work.
