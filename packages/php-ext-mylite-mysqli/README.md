# MyLite mysqli PHP extension

This package builds a PHP extension that registers module `mysqli` and routes a
WordPress-oriented subset of the PHP `mysqli` API to `libmylite`.

Run PHP without the native `mysqli` extension when loading this module:

```sh
cmake --preset storage-smoke-dev -DMYLITE_BUILD_PHP_EXTENSIONS=ON
cmake --build --preset storage-smoke-dev --target mylite_mysqli
php -n -d extension=build/storage-smoke-dev/packages/php-ext-mylite-mysqli/mylite_mysqli.so -m
```

Set `MYLITE_DATABASE_PATH` to choose the `.mylite` file. If it is not set, the
adapter accepts MyLite file paths through the host, socket, or database
arguments; otherwise it derives `<database>.mylite` or uses `:memory:`.

For WordPress bootstrap, keep `DB_NAME` as the SQL schema name and set:

```sh
export MYLITE_DATABASE_PATH=/path/to/wordpress.mylite
```

When running PHPUnit tests that use PHP process isolation, load the extension
through PHP configuration rather than only passing `-d extension=...` to the
parent process, so child PHP processes load the same replacement `mysqli`
module.

The adapter currently buffers result sets and implements the APIs needed for
initial WordPress database access. Unsupported network, SSL, async, persistent
connection, and server-process surfaces return compatibility no-ops or `false`.
