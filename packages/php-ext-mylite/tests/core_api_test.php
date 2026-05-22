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
    $path = sys_get_temp_dir() . '/mylite-php-core-' . getmypid() . '-' . $name . '.mylite';
    remove_tree($path);
    register_shutdown_function('remove_tree', $path);
    return $path;
}

expect_true(extension_loaded('mylite'), 'mylite extension is not loaded');
expect_true(mylite_version() !== '', 'mylite_version() returned an empty version');
expect_true(MYLITE_NOMEM === 7, 'MYLITE_NOMEM constant mismatch');
expect_true(MYLITE_TYPE_TEXT === 4, 'MYLITE_TYPE_TEXT constant mismatch');

$invalidFlagsPath = test_path('invalid-flags');
try {
    mylite_open($invalidFlagsPath, -1);
    throw new RuntimeException('mylite_open() accepted invalid flags');
} catch (MyLite\Exception $exception) {
    expect_true($exception->getCode() === MYLITE_MISUSE, 'mylite_open() invalid flags code mismatch');
}
try {
    new MyLite\Connection($invalidFlagsPath, -1);
    throw new RuntimeException('Connection constructor accepted invalid flags');
} catch (MyLite\Exception $exception) {
    expect_true($exception->getCode() === MYLITE_MISUSE, 'constructor invalid flags code mismatch');
}

$path = test_path('native');
$db = mylite_open($path);
expect_true($db instanceof MyLite\Connection, 'mylite_open() did not return a connection');

expect_true($db->exec('CREATE DATABASE app') >= 0, 'CREATE DATABASE failed');
expect_true($db->exec('USE app') >= 0, 'USE failed');
expect_true($db->exec('CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(32), score DOUBLE) ENGINE=MyISAM') >= 0, 'CREATE TABLE failed');
expect_true($db->exec("INSERT INTO people VALUES (1, 'Ada', 9.5)") === 1, 'INSERT affected rows mismatch');
expect_true($db->changes() === 1, 'changes() mismatch');

$result = $db->query('SELECT id, name, score FROM people ORDER BY id');
expect_true($result instanceof MyLite\Result, 'query() did not return a result object');
expect_true($result->fetchAssociative() === ['id' => 1, 'name' => 'Ada', 'score' => 9.5], 'query row mismatch');
expect_true($result->fetchAssociative() === null, 'result should be exhausted');

$stmt = $db->prepare('INSERT INTO people VALUES (?, ?, ?)');
expect_true($stmt->bindValue(1, 2), 'bindValue(1) failed');
expect_true($stmt->bindValue(2, 'Grace'), 'bindValue(2) failed');
expect_true($stmt->bindValue(3, 10.25), 'bindValue(3) failed');
expect_true($stmt->execute(), 'prepared INSERT failed');

$stmt = $db->prepare('SELECT name, score FROM people WHERE id = ?');
expect_true($stmt->bindValue(1, 2), 'prepared SELECT bind failed');
expect_true($stmt->execute(), 'prepared SELECT execute failed');
expect_true($stmt->fetchAssociative() === ['name' => 'Grace', 'score' => 10.25], 'prepared SELECT row mismatch');
expect_true($stmt->fetchAssociative() === null, 'prepared SELECT should be exhausted');
unset($stmt);

$activeStmt = $db->prepare('SELECT 1 AS value');
try {
    $db->close();
    throw new RuntimeException('close with active statement did not throw');
} catch (MyLite\Exception $exception) {
    expect_true($exception->getCode() === MYLITE_BUSY, 'active statement close should report MYLITE_BUSY');
}
unset($activeStmt);

try {
    $db->query('SELECT * FROM missing_table');
    throw new RuntimeException('invalid SQL did not throw');
} catch (MyLite\Exception $exception) {
    expect_true($exception->getCode() !== MYLITE_OK, 'exception code should report failure');
    expect_true($db->mariadbErrno() > 0, 'MariaDB errno should be populated');
    expect_true($db->sqlState() !== '00000', 'SQLSTATE should report failure');
    expect_true($db->errorMessage() !== '', 'error message should be populated');
}

expect_true($db->close(), 'close failed');
unset($db);

$db = mylite_open($path);
expect_true($db->exec('USE app') >= 0, 'reopen USE failed');
$result = $db->query('SELECT COUNT(*) AS total FROM people');
expect_true($result->fetchAssociative() === ['total' => 2], 'reopen persistence mismatch');
expect_true($db->close(), 'reopen close failed');

remove_tree($path);
