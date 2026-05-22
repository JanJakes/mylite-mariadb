<?php

declare(strict_types=1);

function expect_true(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function remove_tree(string $path): void
{
    if (!file_exists($path)) {
        return;
    }
    $items = new RecursiveIteratorIterator(
        new RecursiveDirectoryIterator($path, FilesystemIterator::SKIP_DOTS),
        RecursiveIteratorIterator::CHILD_FIRST
    );
    foreach ($items as $item) {
        $item->isDir() ? rmdir($item->getPathname()) : unlink($item->getPathname());
    }
    rmdir($path);
}

function test_path(string $name): string
{
    $path = sys_get_temp_dir() . '/mylite-php-pdo-' . getmypid() . '-' . $name . '.mylite';
    remove_tree($path);
    register_shutdown_function('remove_tree', $path);
    return $path;
}

expect_true(extension_loaded('mylite'), 'mylite extension is not loaded');
expect_true(extension_loaded('pdo_mylite'), 'pdo_mylite extension is not loaded');
expect_true(in_array('mylite', PDO::getAvailableDrivers(), true), 'mylite PDO driver is not registered');

$path = test_path('driver');
$pdo = new PDO('mylite:' . $path, null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);

expect_true($pdo->getAttribute(PDO::ATTR_DRIVER_NAME) === 'mylite', 'driver name mismatch');
expect_true($pdo->exec('CREATE DATABASE app') !== false, 'CREATE DATABASE failed');
expect_true($pdo->exec('USE app') !== false, 'USE failed');
expect_true($pdo->exec('CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(32)) ENGINE=InnoDB') !== false, 'CREATE TABLE failed');

$stmt = $pdo->prepare('INSERT INTO people VALUES (?, ?)');
expect_true($stmt instanceof PDOStatement, 'prepare did not return PDOStatement');
expect_true($stmt->execute([1, 'Ada']), 'prepared INSERT failed');
expect_true($pdo->lastInsertId() !== '', 'lastInsertId should return a string');

$stmt = $pdo->prepare('SELECT name FROM people WHERE id = ?');
expect_true($stmt->execute([1]), 'prepared SELECT failed');
expect_true($stmt->fetch() === ['name' => 'Ada'], 'prepared SELECT row mismatch');
expect_true($stmt->fetch() === false, 'prepared SELECT should be exhausted');

expect_true($pdo->beginTransaction(), 'beginTransaction failed');
expect_true($pdo->exec("INSERT INTO people VALUES (2, 'Grace')") === 1, 'transaction INSERT failed');
expect_true($pdo->rollBack(), 'rollBack failed');
expect_true($pdo->query('SELECT COUNT(*) AS total FROM people')->fetch() === ['total' => 1], 'rollback count mismatch');

$expectedEscape = 'A' . '\\0' . '\\n' . '\\r' . '\\\\' . "\\'" . '\\"' . '\\Z' . 'B';
expect_true($pdo->quote("A\0\n\r\\'\"\x1aB") === "'" . $expectedEscape . "'", 'PDO quote mismatch');

$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
expect_true($pdo->exec('INSERT INTO missing_table VALUES (1)') === false, 'silent invalid query should fail');
$errorInfo = $pdo->errorInfo();
expect_true($errorInfo[0] !== '00000', 'PDO errorInfo SQLSTATE mismatch');
expect_true($errorInfo[1] > 0, 'PDO errorInfo native code mismatch');
expect_true($errorInfo[2] !== '', 'PDO errorInfo message mismatch');
$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

try {
    $pdo->exec('INSERT INTO missing_table VALUES (1)');
    throw new RuntimeException('invalid query did not throw');
} catch (PDOException $exception) {
    expect_true($exception->getCode() !== '00000', 'PDOException SQLSTATE mismatch');
}

unset($stmt, $pdo);
remove_tree($path);

$path = test_path('key-value-dsn');
$pdo = new PDO('mylite:path=' . $path);
expect_true($pdo->exec('CREATE DATABASE app') !== false, 'path= CREATE DATABASE failed');
expect_true($pdo->exec('USE app') !== false, 'path= USE failed');
expect_true($pdo->exec('CREATE TABLE kv (id INT PRIMARY KEY) ENGINE=MyISAM') !== false, 'path= DSN failed');
unset($pdo);
remove_tree($path);

try {
    new PDO('mylite:');
    throw new RuntimeException('empty PDO DSN did not throw');
} catch (PDOException $exception) {
    expect_true($exception->getCode() !== '00000', 'empty PDO DSN SQLSTATE mismatch');
}

try {
    new PDO('mylite:' . test_path('persistent'), null, null, [PDO::ATTR_PERSISTENT => true]);
    throw new RuntimeException('persistent PDO connection did not throw');
} catch (PDOException $exception) {
    expect_true($exception->getCode() !== '00000', 'persistent PDO SQLSTATE mismatch');
}
