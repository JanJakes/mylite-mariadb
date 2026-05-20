# Embedded Bootstrap And Open/Close

## Goal

Add the first `libmylite` lifecycle implementation on top of MariaDB's embedded
library: initialize the embedded runtime under MyLite-owned defaults, open a
database handle, and close it without requiring a daemon, socket, network
handshake, or ambient server option files.

This slice may use a temporary MyLite-owned runtime directory until the native
storage directory lifecycle is configured and tested. That directory is
compatibility scaffolding only; durable user state must stay within the MyLite
database directory in the product architecture.

## Non-Goals

- Do not implement SQL execution APIs.
- Do not configure native storage directory lifecycle.
- Do not claim final durable SQL state.
- Do not support server accounts, authentication setup, replication, binlog, or
  dynamic plugins through the core open path.
- Do not expose raw `MYSQL *` handles from the public MyLite API.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/include/mysql.h:381` declares `mysql_server_init()` and
  `mysql_server_end()`. Lines 393-394 alias them as `mysql_library_init()` and
  `mysql_library_end()`.
- `mariadb/include/mysql.h:399` documents `mysql_thread_init()` and
  `mysql_thread_end()` for threads that open at least one connection.
- `mariadb/libmysqld/libmysql.c:126` initializes the client and embedded library
  path from `mysql_server_init()`. Lines 182-185 call `init_embedded_server()`
  when building the embedded library.
- `mariadb/libmysqld/libmysql.c:205` tears down the library and calls
  `end_embedded_server()` for embedded builds.
- `mariadb/libmysqld/lib_sql.cc:532` implements `init_embedded_server()`. It
  creates fake argv/groups when missing, calls `load_defaults()` at line 574,
  initializes common server state at lines 591-617, initializes ACL and grant
  state at lines 624-629, calls the UDF init hooks at lines 639-644, and runs
  DDL recovery before marking the embedded server initialized at lines 663-669.
  The current embedded profile stubs the dynamic UDF runtime; the bootstrap
  source finding remains relevant as a lifecycle hook.
- `mariadb/libmysqld/libmysqld.c:75` implements embedded
  `mysql_real_connect()`. Lines 98-103 route non-local connections to the
  client path, line 105 selects `embedded_methods`, lines 155-157 force port and
  socket to zero, lines 164-167 remove compression and pluggable auth, and
  lines 171-180 create and check the embedded THD.

## Design

`libmylite` should own one process-global embedded runtime. Initialization must
be guarded by a mutex and reference count so multiple `mylite_db` handles for
the same database directory share one MariaDB embedded runtime and the final
close calls `mysql_library_end()` only after all handles are closed. Until a
per-directory runtime model exists, opening a different database directory while
the runtime is active returns `MYLITE_BUSY`.

The bootstrap argv should be constructed by MyLite, not inherited from the host
process. The first implementation should pass `--no-defaults` so MariaDB does
not read ambient option files, then set only the minimum paths and embedded
options needed for startup. The open path should establish the requested MyLite
database directory, while the temporary runtime directory should be created by
MyLite from the requested MyLite path and open configuration. Every path passed
to MariaDB should remain under MyLite ownership.

The public open path should:

- validate `mylite_open()` arguments before touching MariaDB;
- validate open flags and reject conflicting modes;
- reject read-only opens until native storage can enforce read-only access;
- normalize the requested path enough to produce deterministic diagnostics;
- reject a different database directory while the process-global runtime is
  active before creating that directory;
- initialize the process-global embedded runtime when the first handle opens;
- initialize per-thread MariaDB state before using embedded connection APIs;
- create an embedded `MYSQL` connection with local/embedded semantics only;
- store MariaDB errno, SQLSTATE, and error message in MyLite diagnostics on
  failure;
- make `mylite_close()` idempotently release the connection and per-handle
  MyLite allocations.

The first implementation should keep MyLite's handle structure opaque and
private to `packages/libmylite`. Raw MariaDB handles may be stored internally
only as an implementation detail.

## Implementation Notes

The first implementation adds an optional MariaDB embedded backend to
`libmylite`. The normal `dev` preset keeps first-party validation lightweight;
the `embedded-dev` preset links `libmylite` against
`build/mariadb-embedded/libmysqld/libmariadbd.a` and runs the embedded lifecycle
tests.

Runtime startup uses MyLite-owned arguments:

- `--no-defaults`
- `--datadir=<runtime>/data`
- `--tmpdir=<runtime>/tmp`
- `--plugin-dir=<runtime>/plugins`
- `--skip-grant-tables`
- `--skip-networking`
- `--default-storage-engine=MyISAM`
- `--innodb=OFF`
- explicit message and character-set directories from the MariaDB build/source

`--default-storage-engine=MyISAM` and `--innodb=OFF` were temporary bootstrap
choices for this early slice. Later native-storage work keeps MyISAM as the
default engine but enables explicit InnoDB tables with InnoDB paths contained
inside the MyLite database directory.

MariaDB embedded restart required two narrow fork patches:

- `mariadb/sql/mysqld.cc` restores the scheduler function pointers after
  `mysql_server_end()` so a later embedded startup does not call through a null
  scheduler.
- `mariadb/sql/sql_locale.cc` preserves the active error-message table across
  `mysql_server_end()` so the next `init_errmessage()` call can release it
  through MariaDB's existing owner path.

The implementation does not call `mysql_thread_init()` per handle. In the tested
single-threaded embedded path, `mysql_real_connect()` creates and stores the
embedded THD; extra per-handle thread init/end calls corrupted repeated
same-thread lifecycle tests. Multi-threaded handle use remains a later
concurrency slice.

## File Lifecycle

Before native storage directory lifecycle exists, open/create establishes the
requested MyLite database directory but open/close tests may create MariaDB
runtime files inside a temporary MyLite-owned directory. Tests must assert the
runtime directory is either removed on close or its remaining contents are
documented as transient bootstrap debt. The user-provided MyLite path is the
product boundary and must not silently become a server datadir outside MyLite
ownership.

The implemented bootstrap originally removed the temporary runtime directory on
the final close. The native-storage baseline supersedes that for durable
database paths: MariaDB now starts with `--datadir=<db>/datadir`,
`--tmpdir=<db>/tmp`, `--plugin-dir=<db>/run/plugins`, and
`--aria-log-dir-path=<db>/datadir`. `run/` is removed on clean shutdown and
`tmp/` is cleared; durable metadata and table state stay under `datadir/`.

## Compatibility Impact

This slice introduces public API behavior but should not change SQL semantics.
Compatibility evidence should focus on lifecycle behavior:

- no network listener or daemon is required;
- ambient MariaDB option files do not affect `mylite_open()`;
- unsupported server surfaces fail explicitly instead of leaking through
  defaults;
- diagnostics preserve MariaDB errno and SQLSTATE where available;
- close behavior is deterministic. Statement-dependent `MYLITE_BUSY` close
  behavior belongs to the later prepared-statement slice.

Update `docs/COMPATIBILITY.md` when public open/close behavior lands.

## Test Plan

1. Add unit tests for invalid open arguments, invalid flag combinations, missing
   output pointers, and close of `NULL`.
2. Add open/close integration tests that initialize the embedded runtime once,
   open two handles sequentially, and verify final teardown.
3. Add a test proving ambient option files are ignored by the MyLite open path.
4. Add temporary-directory lifecycle assertions for files created by the
   bootstrap scaffolding.
5. Add diagnostics tests for at least one startup failure and one bad path or
   flag failure.
6. Add coverage for `:memory:` open/close, exclusive existing-directory failure,
   and the temporary one-active-directory-per-process runtime limit.

## Acceptance Criteria

- `mylite_open()` is implemented behind the documented public C API shape.
- `MYLITE_OPEN_CREATE` creates the requested MyLite database directory, not a
  placeholder file.
- `MYLITE_OPEN_READONLY` fails explicitly until native storage can enforce it.
- `mylite_close()` releases all per-handle state and tears down the global
  embedded runtime only when the last handle closes.
- A second database directory open returns `MYLITE_BUSY` while the runtime is
  active and does not create the rejected directory.
- The open path uses MyLite-owned argv/options and does not read ambient MariaDB
  option files.
- Tests cover successful open/close, invalid arguments, diagnostics, and
  temporary file lifecycle.
- Documentation clearly marks any temporary datadir scaffolding as non-final
  bootstrap debt.

## Risks And Open Questions

- MariaDB's embedded startup still expects server-style paths and grant state;
  the exact minimal argv/path set must be proven by implementation tests.
- Temporary runtime-directory scaffolding is acceptable only until native
  storage directory lifecycle keeps durable state inside the MyLite database
  directory.
- Thread and process-global initialization must be tested under repeated
  open/close sequences before the API can be treated as stable.
