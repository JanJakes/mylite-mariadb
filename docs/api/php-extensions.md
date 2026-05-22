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
