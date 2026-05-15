# Server Utility Function Trim

## Problem

MyLite's embedded profile rejects server-oriented SQL and host-file SQL I/O at
the public SQL policy boundary, but the default embedded MariaDB archive still
keeps native builders and item implementations for low-value server utility
functions. These functions do not fit the file-owned embedded product shape:
some intentionally block the executing thread, depend on server-level named
locks, derive values from server identity and startup time, or wait for
replication state that MyLite disables.

The bundle-size research records a successful trim for this family of functions.
This slice turns that research into a reviewed profile option, keeps upstream
behavior available by default, and makes public MyLite diagnostics explicit for
the functions affected by the trim.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/item_create.cc:6363`, `6420`, `6426-6427`, `6490-6491`,
  `6519-6521`, `6540`, and `6565` register `BENCHMARK`, named-lock helpers,
  replication wait helpers, `SLEEP`, and `UUID_SHORT` in the native function
  table.
- `mariadb/sql/item_create.cc:6477` registers `LOAD_FILE`; public MyLite policy
  already rejects it and `MYLITE_WITH_SQL_FILE_IO=OFF` already compiles the
  host-file read body to an unsupported stub.
- `mariadb/sql/item_func.cc:4556-4619` implements `BENCHMARK()` by repeatedly
  evaluating another expression.
- `mariadb/sql/item_func.cc:4670-4704` implements `SLEEP()` by pausing query
  execution through MariaDB's interruptible wait path.
- `mariadb/sql/item_func.cc:4302-4530` implements user-level named-lock SQL
  functions around the thread user-lock hash and MDL context.
- `mariadb/sql/item_func.cc:3976-4077` implements `MASTER_POS_WAIT()` and
  `MASTER_GTID_WAIT()` replication wait functions.
- `mariadb/sql/item_func.cc:6977-6996` initializes and exposes the
  `UUID_SHORT()` server-derived counter. `server_uuid_value()` is also used by
  `mariadb/sql/ddl_log.cc`, so the SQL item can be trimmed while the helper
  remains.
- MariaDB documentation describes these as specialized server utilities:
  `BENCHMARK()` repeatedly executes an expression for timing, `SLEEP()` pauses
  execution, `GET_LOCK()` acquires named user locks, `MASTER_POS_WAIT()` and
  `MASTER_GTID_WAIT()` block on replication state, and `UUID_SHORT()` depends
  on `server_id`, server startup time, and a per-server counter.

Official MariaDB references:

- <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/information-functions/benchmark>
- <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/miscellaneous-functions/sleep>
- <https://mariadb.com/kb/en/get_lock/>
- <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/miscellaneous-functions/master_pos_wait>
- <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/miscellaneous-functions/master_gtid_wait>
- <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/miscellaneous-functions/uuid_short>

## Scope

- Add a `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS` CMake option that defaults to
  `ON`.
- Set `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS=OFF` in the MyLite embedded
  profile.
- When the option is off, compile out native SQL builders, native function
  table entries, and SQL item implementations for:
  - `BENCHMARK()`;
  - named-lock helpers: `GET_LOCK()`, `RELEASE_LOCK()`,
    `RELEASE_ALL_LOCKS()`, `IS_FREE_LOCK()`, and `IS_USED_LOCK()`;
  - replication wait helpers: `MASTER_POS_WAIT()` and `MASTER_GTID_WAIT()`;
  - `SLEEP()`;
  - `UUID_SHORT()`.
- When `MYLITE_WITH_SQL_FILE_IO=OFF`, remove the retained `LOAD_FILE()` native
  builder and function-table entry in addition to the existing fail-closed body.
- Add public SQL policy coverage for direct and prepared calls to the server
  utility functions above, while retaining allowed utility functions such as
  `VERSION()`.

## Non-Goals

- Remove parser support for generic function calls.
- Remove ordinary scalar utility functions that are useful in embedded
  application SQL, such as `VERSION()`, `DATABASE()`, or date/math/string
  helpers.
- Remove the internal `server_uuid_value()` helper used outside
  `UUID_SHORT()`.
- Remove user-lock cleanup structures from `THD`; the trim prevents supported
  entry points from creating such locks.
- Implement replacement embedded semantics for sleeps, named locks, server
  benchmarks, or replication waits.

## Design

Define `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS` in both MariaDB SQL build roots:

- `mariadb/sql/CMakeLists.txt` for the normal `sql` target;
- `mariadb/libmysqld/CMakeLists.txt` for the embedded `sql_embedded` archive.

When enabled, MariaDB source remains unchanged. When disabled:

1. `item_create.cc` omits the native function builder classes, singleton
   definitions, builder bodies, and function-table entries for the trimmed
   utility functions.
2. `item_func.h` omits the corresponding SQL item classes.
3. `item_func.cc` omits the corresponding SQL item method bodies while keeping
   shared infrastructure that retained MariaDB code still references.
4. `LOAD_FILE()` builder registration is tied to the existing
   `MYLITE_WITH_SQL_FILE_IO` option.
5. `libmylite` rejects direct and prepared calls before MariaDB execution with
   stable MyLite diagnostics.

This keeps upstream-style builds compatible by default and confines the MyLite
profile delta to explicit preprocessor gates around already-unsupported
surfaces.

## Compatibility Impact

Supported MyLite SQL does not lose application-facing behavior. The trimmed
functions either already belonged to unsupported server surfaces, replication,
named locking, or host-file I/O, or are low-value server utility behavior that
does not match a local file-owned API.

Direct and prepared public entry points return a stable MyLite error before
MariaDB execution for the newly covered utility functions. Internal bypasses in
the trimmed embedded profile see missing native functions rather than executing
server-oriented behavior.

## Single-File And Embedded-Lifecycle Impact

The trim removes reachable server utility execution paths from the default
embedded profile and does not add durable files, sidecars, background threads,
or persistent runtime state.

## Public API And File-Format Impact

No public C API or `.mylite` file-format changes.

## Storage-Engine Routing Impact

None. The slice affects SQL function construction before storage-engine
routing.

## Wire-Protocol Or Integration-Package Impact

None for core `libmylite`. A future wire-protocol adapter that wants
server-compatible sleeps, named locks, or replication waits would need an
adapter-level policy and implementation.

## Binary-Size Impact

`docs/architecture/bundle-size-research.md` records prior evidence for this
family at 37,576 bytes from the embedded archive and 228,044 bytes from linked
artifacts. Remeasured on 2026-05-15 after implementation:

- default embedded archive: 31,926,704 bytes / 30.45 MiB, 692 members;
- storage-smoke archive: 32,107,288 bytes / 30.62 MiB, 695 members;
- compared with the previous committed profile, both archives are 90,936 bytes
  smaller, and representative stripped linked smoke binaries are about 59 KiB
  smaller.

The current delta is smaller than the old research result because the branch
already trimmed adjacent file-I/O surfaces and because some user-lock and
sleep-wait infrastructure remains referenced by retained MariaDB internals.

## License Or Dependency Impact

No new dependencies and no license change.

## Test And Verification Plan

- Add direct execution policy tests for `BENCHMARK()`, `SLEEP()`,
  `UUID_SHORT()`, `MASTER_POS_WAIT()`, and `MASTER_GTID_WAIT()`.
- Keep named-lock rejection coverage under the locking policy.
- Add prepared-statement policy coverage for representative server utility
  functions.
- Verify quoted function names do not trigger policy rejection.
- Verify `VERSION()` still executes.
- Reconfigure and rebuild the default embedded MariaDB archive and the
  storage-smoke MariaDB archive.
- Build and run embedded and storage-smoke presets.
- Run `tools/mylite-compat-harness report server-surface`.
- Run `tools/mylite-size-report` and archive `measure` commands.
- Run formatting, shell syntax, whitespace, and first-party tidy checks.

## Acceptance Criteria

- The default MyLite embedded profile records
  `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS=OFF`.
- Public direct and prepared entry points reject the trimmed server utility
  functions with stable MyLite diagnostics before MariaDB execution.
- `VERSION()` and ordinary scalar SQL continue to work.
- The normal MariaDB SQL target still builds with the new option in the current
  profile.
- Size documentation records the current measured impact.

## Risks And Open Questions

- MariaDB keeps some user-lock cleanup and sleep-wait infrastructure for other
  internal code paths, so the final measured size may be lower than the
  historical research delta.
- Applications that happen to call `SLEEP()` or `BENCHMARK()` through MyLite
  will now see explicit MyLite policy errors. That is intentional because these
  functions are operational/server utilities rather than embedded storage
  behavior.
