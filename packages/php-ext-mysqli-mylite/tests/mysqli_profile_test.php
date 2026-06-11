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

$stmt = $db->prepare('INSERT INTO profile_notes VALUES (?, ?)');
expect_true($stmt instanceof MyLite\MySQLiStmt, 'prepare did not return statement');
$id = 2;
$body = 'second';
expect_true($stmt->bind_param('is', $id, $body), 'bind_param failed');
expect_true($stmt->execute(), 'execute failed');

unset($result, $stmt);
expect_true($db->close(), 'close failed');
