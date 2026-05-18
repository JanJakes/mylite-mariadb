# PHP mysqli extension

## Problem

WordPress reaches MySQL/MariaDB through PHP's `mysqli` extension. MyLite already
has a file-owned `libmylite` C API, but there is no PHP-facing adapter that can
stand in for `mysqli` without a daemon, socket, or MySQL client handshake.

This slice adds a first-party PHP extension package under
`packages/php-ext-mylite-mysqli` that registers the public `mysqli` functions
and classes when native PHP `mysqli` is not loaded, then maps the WordPress-sized
subset to embedded `libmylite` handles.

## Source findings

- MyLite targets MariaDB 11.8 LTS, initial import ref `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB field type and flag constants are defined in
  `mariadb/include/mysql_com.h`; `libmylite` exposes the same field metadata
  through `mylite_column_mariadb_type()`, `mylite_column_flags()`,
  `mylite_column_charset()`, lengths, decimals, and origin-name accessors.
- PHP's installed `mysqli` extension exposes the runtime shape WordPress expects:
  `mysqli`, `mysqli_result`, `mysqli_stmt`, `mysqli_driver`,
  `mysqli_warning`, and `mysqli_sql_exception`, plus `MYSQLI_*` constants.
  The local PHP 8.5 reflection output and headers under
  `/opt/homebrew/Cellar/php/8.5.5_1/include/php/ext/mysqli/` were used for the
  initial method and property set.
- `libmylite` remains the primary lifetime boundary. The extension must not
  expose or depend on raw `MYSQL *` handles.

## Scope

- Build a loadable PHP extension module named `mysqli` from package sources.
- Support object-oriented and procedural APIs used by WordPress database access:
  connect/init/real_connect, query/real_query/store_result/use_result,
  result fetch modes, escaping, charset selection, error properties,
  affected rows, insert ids, transactions, and prepared statements with
  `bind_param()`, `execute()`, `bind_result()`, and `get_result()`.
- Resolve a MyLite database filename from `MYLITE_DATABASE_PATH`, MyLite-style
  host/socket path hints, or the PHP database name. The SQL schema name remains
  the `mysqli` database argument when it is not a path.
- Add a CTest smoke that loads the extension with `php -n`, creates a
  WordPress-shaped options table, performs ordinary and prepared DML, and
  verifies result fetching.

## Non-goals

- Full native `mysqli` parity for async queries, persistent network links, SSL,
  MySQL protocol statistics, server process control, or real client/server
  connection attributes.
- Loading alongside native PHP `mysqli`; PHP processes should disable the
  native extension and load this module for MyLite tests.
- Server authentication. User, password, port, and SSL options are accepted for
  signature compatibility but do not participate in MyLite open.

## Design

The extension owns PHP objects and holds opaque `mylite_db` / `mylite_stmt`
handles internally. Direct queries use `mylite_prepare()` / `mylite_step()` so
the adapter can collect field metadata and row values consistently. Non-result
statements update affected-row and insert-id properties from `libmylite`.

`mysqli_result` is buffered. This matches the default WordPress path and keeps
the initial adapter simple: `MYSQLI_USE_RESULT` is accepted as a compatibility
mode but still materializes rows. `mysqli_stmt` stores parameter references and
binds them immediately before each execution.

When the PHP database argument is a schema name, `real_connect()` creates the
schema if needed and selects it. This keeps embedded WordPress bootstrap
practical while retaining ordinary `USE db` semantics after connect.

## Compatibility impact

This is an integration package, not a change to SQL behavior. MySQL/MariaDB SQL,
metadata, diagnostics, warnings, affected rows, and insert ids still come from
`libmylite`. The adapter declares the common `MYSQLI_*` constants with values
matching the local PHP `mysqli` reflection output.

Unsupported `mysqli` surfaces return stable false/no-op compatibility results
where WordPress does not require the feature. They should be expanded only when
an application test needs them.

## Single-file and lifecycle impact

Each connected `mysqli` object opens one `mylite_db` file handle. Durable state
stays in the selected `.mylite` file. No socket, daemon process, or native
server datadir is created by the extension.

## Build, dependency, and license impact

The package depends on PHP headers from `php-config` and links the first-party
`MyLite::mylite` target. No new third-party runtime dependency is vendored.
The package is GPL-2.0 with the rest of MyLite.

`MYLITE_BUILD_PHP_EXTENSIONS` is opt-in so the default CMake developer build
does not require PHP headers on every machine.

## Verification plan

- Configure and build the normal developer preset to ensure the package remains
  opt-in.
- Configure with `MYLITE_BUILD_PHP_EXTENSIONS=ON` and
  `MYLITE_WITH_MARIADB_EMBEDDED=ON` after the embedded MariaDB archive exists.
- Run the PHP smoke test through CTest with `php -n -d extension=<module>`.
- Run formatting over first-party sources.

## Acceptance criteria

- `php -n -d extension=.../mylite_mysqli.so` reports `extension_loaded('mysqli')`.
- Basic procedural and object-oriented `mysqli` query and result fetch APIs work
  against a MyLite file.
- Prepared statement binding and `get_result()` work for representative
  WordPress-style option rows.
- The package is documented and committed as an atomic checkpoint.

## Risks

- Native PHP `mysqli` has a broad API. The initial adapter is intentionally
  WordPress-oriented and will need expansion as real WordPress paths identify
  missing behavior.
- PHP extension ABI details differ across PHP releases. The package is built
  against the active `php-config` and does not yet provide a phpize installer.
