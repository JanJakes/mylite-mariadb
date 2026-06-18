# PHP Extensions

MyLite PHP integration uses one core runtime extension plus API extensions that
depend on it.

## Packages

| Package | PHP module | Purpose |
| --- | --- | --- |
| `packages/php-ext-mylite` | `mylite` | Owns the loaded `libmylite` runtime and exposes a small native PHP API. |
| `packages/php-ext-mysqli-mylite` | `mysqli_mylite` | Exposes a MyLite-backed mysqli-shaped API. |
| `packages/php-ext-pdo-mylite` | `pdo_mylite` | Registers a PDO driver named `mylite`. |

Only `mylite.so` links `libmylite`. API extensions require `mylite` and call
the loaded core extension's exported `mylite_*` symbols so one PHP process does
not load duplicate embedded MariaDB runtimes.

## Build

The PHP packages are opt-in because they require `php-config`, PHP headers, and
a PHP CLI matching the target ABI.

```sh
tools/mariadb-embedded-build all
cmake --preset php-embedded-dev
cmake --build --preset php-embedded-dev
ctest --preset php-embedded-dev -L php
```

## Native MyLite API

The native API is intentionally small:

```php
$db = mylite_open('/path/app.mylite');
$db->exec('CREATE DATABASE app');
$db->exec('USE app');
$db->exec('CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(32)) ENGINE=InnoDB');

$stmt = $db->prepare('INSERT INTO people VALUES (?, ?)');
$stmt->bindValue(1, 1);
$stmt->bindValue(2, 'Ada');
$stmt->execute();

$result = $db->query('SELECT name FROM people WHERE id = 1');
$row = $result->fetchAssociative();
$db->close();
```

The API exposes `MyLite\Connection`, `MyLite\Statement`, `MyLite\Result`, and
`MyLite\Exception`.

## mysqli-Shaped API

`mysqli_mylite` always exposes namespaced classes and functions:

```php
$db = new MyLite\MySQLi('/path/app.mylite');
$db->query('CREATE DATABASE app');
$db->query('USE app');
$result = $db->query('SELECT 1 AS value');
$row = $result->fetch_assoc();
```

Procedural helpers are available under the `MyLite` namespace, such as
`MyLite\mysqli_connect()`, `MyLite\mysqli_query()`, and
`MyLite\mysqli_fetch_assoc()`.

Global `mysqli`, `mysqli_result`, `mysqli_stmt`, and `mysqli_*` symbols are
registered only when stock PHP `ext/mysqli` is not already loaded. Most
developer PHP builds load stock `mysqli`, so replacement mode must be tested in
a PHP runtime built without it.

The mysqli host argument is interpreted as the MyLite database directory path.
User, password, port, socket, and server authentication parameters do not start
a network connection.

For performance attribution runs, `mysqli_mylite` can emit process-local
adapter counters when `MYLITE_MYSQLI_PROFILE=1` is present in the PHP process
environment. The summary is printed at module shutdown with
`mylite_mysqli_profile_*` keys covering open/close calls, direct
`mysqli_query()` paths, query classification, result-statement cache handling,
prepared-statement execution, result stepping, row and field materialization,
status synchronization, result-object creation, and result-fetch counts plus
fetch elapsed time. Diagnostic harnesses may set
`MYLITE_MYSQLI_PROFILE_CONTEXT` to add a sanitized
`mylite_mysqli_profile_context=<value>` line before the counters, allowing
multi-process profile blocks to be grouped without changing adapter behavior.
Normal runs leave this disabled.

The adapter routes first-seen and non-repeated result-producing
`mysqli_query()` calls through libmylite's direct text-result API, which
preserves display/original field and table metadata while avoiding
prepared-statement prepare/finalize cost for ordinary unique result queries.
Immediate exact repeats promote to the prepared-result cache so tight repeated
loops can still amortize prepare cost. Values are copied with explicit byte
lengths, so embedded NULs in binary results are preserved. `mysqli::prepare()`
and `mysqli_stmt` execution still use MariaDB prepared statements. The older
always-prepared result route remains available for diagnostics with
`MYLITE_MYSQLI_PREPARED_QUERY_RESULTS=1`; ordinary no-result `INSERT`,
`UPDATE`, `DELETE`, and `REPLACE` statements without `RETURNING` preserve that
cache, while DDL, schema, transaction, lock, `SET`, `USE`, `CALL`, and error
paths clear it conservatively.

The WordPress PHPUnit harness also has an opt-in
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1` mode for production timing runs. That
mode opens one harness-owned mysqli connection after WordPress bootstrap and
closes it during process shutdown so short-lived WordPress mysqli objects do
not repeatedly cold-shutdown the embedded runtime. The harness also releases
and reopens that connection around PHPUnit child processes. It is harness-only
and does not change `mysqli_close()` or `mylite_close()` semantics.

## PDO Driver

`pdo_mylite` registers the PDO driver name `mylite`:

```php
$pdo = new PDO('mylite:/path/app.mylite');
$pdo = new PDO('mylite:path=/path/app.mylite');
```

The first driver supports direct execution, queries, transactions, quoting,
`lastInsertId()`, SQLSTATE/errorInfo, and native prepared statements with
positional placeholders. Persistent PDO connections and named placeholders are
not supported yet.
