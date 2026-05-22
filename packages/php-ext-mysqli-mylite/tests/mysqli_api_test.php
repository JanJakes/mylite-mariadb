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
    $path = sys_get_temp_dir() . '/mylite-php-mysqli-' . getmypid() . '-' . $name . '.mylite';
    remove_tree($path);
    register_shutdown_function('remove_tree', $path);
    return $path;
}

expect_true(extension_loaded('mylite'), 'mylite extension is not loaded');
expect_true(extension_loaded('mysqli_mylite'), 'mysqli_mylite extension is not loaded');
expect_true(is_bool(MyLite\mysqli_mylite_global_symbols_enabled()), 'global symbol flag should be boolean');

try {
    MyLite\mysqli_query(new stdClass(), 'SELECT 1');
    throw new RuntimeException('procedural query accepted a non-MyLite object');
} catch (TypeError $exception) {
    expect_true($exception->getMessage() !== '', 'procedural query TypeError should have a message');
}

$path = test_path('object');
$db = new MyLite\MySQLi($path);
expect_true($db->errno === 0, 'initial mysqli errno mismatch');
expect_true($db->query('CREATE DATABASE app') === true, 'CREATE DATABASE failed');
expect_true($db->query('USE app') === true, 'USE failed');
expect_true($db->query('CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(32)) ENGINE=MyISAM') === true, 'CREATE TABLE failed');
expect_true($db->query("INSERT INTO people VALUES (1, 'Ada')") === true, 'INSERT failed');
expect_true((int)$db->affected_rows === 1, 'affected_rows mismatch');

$result = $db->query('SELECT id, name FROM people ORDER BY id');
expect_true($result instanceof MyLite\MySQLiResult, 'query did not return MySQLiResult');
expect_true($result->num_rows === 1, 'num_rows property mismatch');
expect_true($result->fetch_assoc() === ['id' => 1, 'name' => 'Ada'], 'fetch_assoc row mismatch');
expect_true($result->fetch_assoc() === null, 'result should be exhausted');
$expectedEscape = 'A' . '\\0' . '\\n' . '\\r' . '\\\\' . "\\'" . '\\"' . '\\Z' . 'B';
expect_true($db->real_escape_string("A\0\n\r\\'\"\x1aB") === $expectedEscape, 'real_escape_string mismatch');

$stmt = $db->prepare('INSERT INTO people VALUES (?, ?)');
$id = 2;
$name = 'Grace';
expect_true($stmt instanceof MyLite\MySQLiStmt, 'prepare did not return MySQLiStmt');
expect_true($stmt->bind_param('is', $id, $name), 'bind_param failed');
$id = 3;
$name = 'Katherine';
expect_true($stmt->execute(), 'statement execute failed');

$stmt = $db->prepare('SELECT name FROM people WHERE id = ?');
$id = 3;
expect_true($stmt->bind_param('i', $id), 'SELECT bind_param failed');
expect_true($stmt->execute(), 'SELECT execute failed');
$result = $stmt->get_result();
expect_true($result instanceof MyLite\MySQLiResult, 'get_result did not return result');
expect_true($result->fetch_assoc() === ['name' => 'Katherine'], 'prepared get_result row mismatch');

expect_true($db->query('SELECT * FROM missing_table') === false, 'invalid query should fail');
expect_true($db->errno > 0, 'mysqli errno should be populated');
expect_true($db->error !== '', 'mysqli error should be populated');
unset($stmt, $result);
expect_true($db->close(), 'mysqli close failed');
unset($db);

$path = test_path('procedural');
$db = MyLite\mysqli_connect($path);
expect_true($db instanceof MyLite\MySQLi, 'procedural connect failed');
expect_true(MyLite\mysqli_query($db, 'CREATE DATABASE app') === true, 'procedural CREATE DATABASE failed');
expect_true(MyLite\mysqli_query($db, 'USE app') === true, 'procedural USE failed');
expect_true(MyLite\mysqli_query($db, 'CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR(32)) ENGINE=MyISAM') === true, 'procedural CREATE failed');
expect_true(MyLite\mysqli_query($db, "INSERT INTO notes VALUES (1, 'hello')") === true, 'procedural INSERT failed');
$result = MyLite\mysqli_query($db, 'SELECT body FROM notes');
expect_true(MyLite\mysqli_num_rows($result) === 1, 'procedural num_rows mismatch');
expect_true(MyLite\mysqli_fetch_assoc($result) === ['body' => 'hello'], 'procedural fetch mismatch');
expect_true(MyLite\mysqli_close($db), 'procedural close failed');

remove_tree($path);

if (MyLite\mysqli_mylite_global_symbols_enabled()) {
    $path = test_path('global');
    $db = mysqli_connect($path);
    expect_true($db instanceof mysqli, 'global mysqli_connect did not return mysqli');
    expect_true(mysqli_query($db, 'CREATE DATABASE app') === true, 'global CREATE DATABASE failed');
    expect_true(mysqli_query($db, 'USE app') === true, 'global USE failed');
    expect_true(mysqli_query($db, 'CREATE TABLE one (id INT PRIMARY KEY) ENGINE=MyISAM') === true, 'global CREATE TABLE failed');
    expect_true(mysqli_close($db), 'global close failed');
    remove_tree($path);
}
