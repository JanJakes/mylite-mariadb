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

$path = sys_get_temp_dir() . '/mylite-php-mysqli-profile-' . getmypid() . '.mylite';
remove_tree($path);
register_shutdown_function('remove_tree', $path);

$db = new MyLite\MySQLi($path);
expect_true($db->query('CREATE DATABASE app') === true, 'CREATE DATABASE failed');
expect_true($db->query('USE app') === true, 'USE failed');
expect_true($db->query('SET autocommit = 0') === true, 'SET autocommit off failed');
expect_true($db->query('SET autocommit = 0') === true, 'SET autocommit off no-op failed');
expect_true($db->query('START TRANSACTION') === true, 'START TRANSACTION failed');
expect_true($db->query('COMMIT') === true, 'COMMIT failed');
expect_true($db->query('START TRANSACTION') === true, 'second START TRANSACTION failed');
expect_true($db->query('SAVEPOINT profile_probe') === true, 'SAVEPOINT failed');
expect_true($db->query('RELEASE SAVEPOINT profile_probe') === true, 'RELEASE SAVEPOINT failed');
expect_true($db->query('ROLLBACK') === true, 'ROLLBACK failed');
expect_true($db->query('SET autocommit = 1') === true, 'SET autocommit on failed');
expect_true($db->query('SET autocommit = 1') === true, 'SET autocommit on no-op failed');
expect_true(
    $db->query(
        'CREATE TABLE profile_notes (id INT PRIMARY KEY, body VARCHAR(32)) ENGINE=MyISAM'
    ) === true,
    'CREATE TABLE failed'
);
expect_true($db->query("INSERT INTO profile_notes VALUES (1, 'first')") === true, 'INSERT failed');

$result = $db->query('SELECT body FROM profile_notes WHERE id = 1');
expect_true($result instanceof MyLite\MySQLiResult, 'SELECT did not return a result');
expect_true($result->fetch_assoc() === ['body' => 'first'], 'SELECT row mismatch');

$cachedSql = 'SELECT body FROM profile_notes ORDER BY id';
$result = $db->query($cachedSql);
expect_true($result instanceof MyLite\MySQLiResult, 'cached SELECT did not return a result');
expect_true($result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first']], 'cached SELECT initial row mismatch');
expect_true($db->query("INSERT INTO profile_notes VALUES (2, 'second')") === true, 'second INSERT failed');
$result = $db->query($cachedSql);
expect_true(
    $result instanceof MyLite\MySQLiResult,
    'cached SELECT after DML did not return a result'
);
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first'], ['body' => 'second']],
    'cached SELECT after DML row mismatch'
);

$stmt = $db->prepare('INSERT INTO profile_notes VALUES (?, ?)');
expect_true($stmt instanceof MyLite\MySQLiStmt, 'prepare did not return statement');
$id = 3;
$body = 'third';
expect_true($stmt->bind_param('is', $id, $body), 'bind_param failed');
expect_true($stmt->execute(), 'execute failed');

$result = $db->query('SELECT body FROM profile_notes WHERE id = 3');
expect_true($result instanceof MyLite\MySQLiResult, 'second SELECT did not return a result');
$row = $result->fetch_object();
expect_true(is_object($row) && $row->body === 'third', 'fetch_object row mismatch');

$result = $db->query('SELECT body FROM profile_notes WHERE id = 3');
expect_true($result instanceof MyLite\MySQLiResult, 'third SELECT did not return a result');
$row = $result->fetch_array();
expect_true($row['body'] === 'third' && $row[0] === 'third', 'fetch_array row mismatch');

$transactionCachedSql = 'SELECT body FROM profile_notes WHERE id = 1';
$result = $db->query($transactionCachedSql);
expect_true($result instanceof MyLite\MySQLiResult, 'transaction cached initial SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first']],
    'transaction cached initial row mismatch'
);
expect_true($db->query('START TRANSACTION') === true, 'transaction cache START failed');
expect_true($db->query('ROLLBACK') === true, 'transaction cache ROLLBACK failed');
$result = $db->query($transactionCachedSql);
expect_true($result instanceof MyLite\MySQLiResult, 'transaction cached repeated SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first']],
    'transaction cached repeated row mismatch'
);
expect_true($db->query('START TRANSACTION') === true, 'transaction cache second START failed');
expect_true($db->query('ROLLBACK') === true, 'transaction cache second ROLLBACK failed');
$result = $db->query($transactionCachedSql);
expect_true($result instanceof MyLite\MySQLiResult, 'transaction cached hit SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first']],
    'transaction cached hit row mismatch'
);

$interleavedOne = 'SELECT body FROM profile_notes WHERE id IN (1, 3) ORDER BY id';
$interleavedTwo = 'SELECT id FROM profile_notes WHERE id IN (2, 3) ORDER BY id';
$result = $db->query($interleavedOne);
expect_true($result instanceof MyLite\MySQLiResult, 'first interleaved SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first'], ['body' => 'third']],
    'first interleaved SELECT row mismatch'
);
$result = $db->query($interleavedTwo);
expect_true($result instanceof MyLite\MySQLiResult, 'second interleaved SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['id' => '2'], ['id' => '3']],
    'second interleaved SELECT row mismatch'
);
$result = $db->query($interleavedOne);
expect_true($result instanceof MyLite\MySQLiResult, 'first repeated interleaved SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['body' => 'first'], ['body' => 'third']],
    'first repeated interleaved SELECT row mismatch'
);
$result = $db->query($interleavedTwo);
expect_true($result instanceof MyLite\MySQLiResult, 'second repeated interleaved SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [['id' => '2'], ['id' => '3']],
    'second repeated interleaved SELECT row mismatch'
);

$result = $db->query('SELECT body FROM profile_notes ORDER BY id');
expect_true($result instanceof MyLite\MySQLiResult, 'fourth SELECT did not return a result');
$rows = $result->fetch_all(MYSQLI_ASSOC);
expect_true(
    $rows === [['body' => 'first'], ['body' => 'second'], ['body' => 'third']],
    'fetch_all row mismatch'
);

unset($result, $stmt);
expect_true($db->close(), 'close failed');
