# PHP Extension Packages

## Goal

Add PHP integration packages in this order:

1. `packages/php-ext-mylite`: a loadable `mylite` extension that owns the
   process-loaded `libmylite` runtime and may expose a small native PHP API.
2. `packages/php-ext-mysqli-mylite`: a `mysqli`-shaped API backed by the
   loaded `mylite` extension.
3. `packages/php-ext-pdo-mylite`: a PDO driver named `mylite` backed by the
   loaded `mylite` extension.

The integration must open one MyLite-owned `.mylite` database directory,
execute SQL in-process, and keep sibling PHP extensions from linking duplicate
copies of the embedded MariaDB runtime.

## Non-Goals

- Do not add a daemon, socket, network handshake, server accounts, or password
  authentication to the PHP integration.
- Do not expose raw `MYSQL *` or MariaDB embedded handles to PHP code.
- Do not make the first `mysqli_mylite` package coexist as the global
  `mysqli` replacement when stock `ext/mysqli` is already loaded.
- Do not add persistent PHP connections. MyLite lifecycle and directory locking
  need explicit request-owned handles first.
- Do not add PHP UDF, collation, async, multi-query, or streaming LOB support.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MyLite public API: `packages/libmylite/include/mylite/mylite.h`.
- MyLite embedded implementation:
  `packages/libmylite/src/database.cc:mylite_open`,
  `mylite_exec`, `mylite_prepare`, `mylite_step`, and diagnostics APIs.
- PHP source reference: `php-src` master
  `c4105b6243e8c0ae6882e847414df5930ac45ac7`.
- PHP SQLite references:
  `ext/sqlite3/config0.m4`, `ext/sqlite3/sqlite3.c`,
  `ext/pdo_sqlite/config.m4`, `ext/pdo_sqlite/pdo_sqlite.c`,
  `ext/pdo_sqlite/sqlite_driver.c`, and
  `ext/pdo_sqlite/sqlite_statement.c`.
- PHP PDO driver ABI reference:
  `ext/pdo/php_pdo_driver.h`.
- PHP module dependency reference:
  `Zend/zend_modules.h`.

PHP's SQLite integration uses separate `sqlite3` and `pdo_sqlite` extensions
that both link to external `libsqlite3`. MyLite should not copy that shape
directly because `libmylite` embeds a heavier process-global MariaDB runtime.
The core `mylite` extension should be loaded first and export the `mylite_*`
C symbols; sibling API extensions depend on it and call those symbols without
linking their own `libmylite` archive.

PDO has a first-class driver registration model through
`php_pdo_register_driver()`. A MyLite PDO package can register `mylite` as the
driver name and accept DSNs such as `mylite:/path/app.mylite` and
`mylite:path=/path/app.mylite`.

The stock PHP `mysqli` extension registers global functions and classes named
`mysqli`, `mysqli_result`, and `mysqli_stmt`. A MyLite-backed replacement
cannot claim those names in a PHP runtime where stock `ext/mysqli` is already
loaded. The package can still expose a compatible namespaced surface for normal
test coverage and register global drop-in aliases only when the names are free.

## Compatibility Impact

The slice adds PHP API compatibility surfaces around the existing `libmylite`
SQL behavior. It does not change MariaDB SQL compatibility, native storage
files, or server-surface policy.

`php-ext-mylite` is a MyLite-native PHP API, not a MySQL/MariaDB compatibility
API.

`php-ext-mysqli-mylite` is `mysqli`-shaped but initially bounded to the
serverless path-as-host convention: the connection host argument is interpreted
as the MyLite database directory. User, password, port, socket, and server
authentication parameters are ignored or stored only for API compatibility.
Global drop-in names are registered only in PHP runtimes built without stock
`ext/mysqli`.

`php-ext-pdo-mylite` implements the PDO driver surface through native MyLite
prepared statements and positional placeholders.

## Design

`packages/php-ext-mylite` builds `mylite.so` and links `MyLite::mylite`. It
exports the public `mylite_*` C symbols from that one module and exposes:

- `mylite_version()`,
- `mylite_open()`,
- `MyLite\Connection`,
- `MyLite\Statement`,
- `MyLite\Result`,
- `MyLite\Exception`,
- MyLite result, flag, and value-type constants.

The native PHP API is intentionally small. It exists to prove module loading,
PHP object lifetime, open/close behavior, execution, prepared statements,
diagnostics, and result conversion.

`packages/php-ext-mysqli-mylite` builds `mysqli_mylite.so`, declares a required
module dependency on `mylite`, includes the public MyLite header, and leaves
`mylite_*` unresolved at link time so the loaded `mylite` module owns the
runtime. It exposes `MyLite\MySQLi`, `MyLite\MySQLiResult`,
`MyLite\MySQLiStmt`, and namespaced procedural helpers. If the stock global
mysqli symbols are absent, it also registers global `mysqli_*` functions and
global classes.

`packages/php-ext-pdo-mylite` builds `pdo_mylite.so`, declares required module
dependencies on `pdo` and `mylite`, and registers a PDO driver named `mylite`.
It accepts `mylite:/path/app.mylite` and `mylite:path=/path/app.mylite`.
Persistent connections are rejected.

## File Lifecycle

PHP integrations do not create durable files outside the MyLite database
directory. All durable state remains owned by `libmylite` under the configured
database directory. Tests use temporary `.mylite` directories and remove them
after each PHP process exits.

## Embedded Lifecycle And API

Each PHP connection object owns one `mylite_db *`. Close is explicit and object
destructors close remaining handles. Statement destructors finalize remaining
`mylite_stmt *` handles before their owning connection is closed.

Errors from MyLite become PHP exceptions in the native API, mysqli-style error
properties in the mysqli package, and PDO SQLSTATE/errorInfo data in the PDO
driver. The API packages must not hide `MYLITE_BUSY`, directory corruption, or
SQL diagnostics behind generic PHP failures.

## Build, Size, And Dependencies

The PHP packages require `php-config`, PHP headers, and a PHP CLI for tests.
They are opt-in CMake targets so ordinary `dev` and `embedded-dev` builds do
not require a PHP development package. The `php-embedded-dev` preset enables
the extensions against the embedded MyLite build.

No new third-party library is added. The only new runtime dependency is the
target PHP ABI. The core PHP extension links the already-built embedded MyLite
archive. Sibling API extensions do not link `libmylite`, preventing duplicate
embedded MariaDB runtimes in one PHP process.

## Test Plan

1. Build `mylite.so`, `mysqli_mylite.so`, and `pdo_mylite.so` with CMake.
2. Run PHP CLI tests that load the extensions in dependency order.
3. Cover the native PHP API:
   - module load and version reporting,
   - open/close of a `.mylite` directory,
   - DDL/DML persistence,
   - direct result rows,
   - prepared positional binds,
   - typed fetch values,
   - diagnostics and SQL failure behavior.
4. Cover the mysqli-shaped API:
   - namespaced object API,
   - namespaced procedural API,
   - query/fetch/num_rows,
   - prepared `bind_param()` / `execute()` / `get_result()`,
   - affected rows, insert id, and error fields,
   - explicit detection of whether global replacement names were available.
5. Cover the PDO driver:
   - `PDO::getAvailableDrivers()`,
   - `new PDO('mylite:/path/app.mylite')`,
   - `exec`, `query`, `prepare`, positional binds, transactions,
   - `lastInsertId`, `errorInfo`, and `PDO::quote`.
6. Run format, tidy, full embedded tests, and the PHP extension CTest group.

## Acceptance Criteria

- `packages/php-ext-mylite`, `packages/php-ext-mysqli-mylite`, and
  `packages/php-ext-pdo-mylite` build as PHP modules.
- Only `mylite.so` links `MyLite::mylite`; sibling API modules depend on
  `mylite` and do not link a duplicate embedded archive.
- PHP tests prove the native API, mysqli-shaped API, and PDO driver against a
  real embedded MyLite database directory.
- Documentation states the package names, module names, DSN convention,
  mysqli replacement-mode constraint, and build command.

## Risks And Open Questions

- Global mysqli drop-in behavior needs a PHP runtime without stock `ext/mysqli`
  loaded. The package must report whether replacement mode was enabled and keep
  namespaced tests runnable on normal developer PHP builds.
- The first PDO driver supports positional placeholders. Named placeholders can
  be added later through PDO's SQL parser/rewrite support.
- PHP ABI compatibility is per PHP module API number. Distribution packaging
  must build per supported PHP minor line.
