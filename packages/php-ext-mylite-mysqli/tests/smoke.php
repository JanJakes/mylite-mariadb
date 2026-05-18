<?php

function fail(string $message): void
{
    fwrite(STDERR, $message . PHP_EOL);
    exit(1);
}

function assert_true(bool $condition, string $message): void
{
    if (!$condition) {
        fail($message);
    }
}

$path = tempnam(sys_get_temp_dir(), 'mylite-mysqli-');
if ($path === false) {
    fail('tempnam failed');
}
unlink($path);
$path .= '.mylite';
putenv('MYLITE_DATABASE_PATH=' . $path);

mysqli_report(MYSQLI_REPORT_OFF);

if (!defined('MYLITE_MYSQLI')) {
    fwrite(STDERR, "native mysqli is already loaded; rerun with a PHP build where mysqli is loadable as a shared replacement\n");
    exit(77);
}

$db = mysqli_connect(null, null, null, 'wp_smoke');
assert_true($db instanceof mysqli, 'mysqli_connect did not return a mysqli object');
assert_true(version_compare($db->server_info, '5.5.5', '>='), 'server_info is not WordPress-compatible');
assert_true(mysqli_get_server_version($db) >= 50505, 'server_version is not WordPress-compatible');
assert_true(mysqli_select_db($db, 'wp_smoke'), mysqli_error($db));
assert_true(mysqli_set_charset($db, 'utf8mb4'), mysqli_error($db));
assert_true(mysqli_query($db, 'SET default_storage_engine = InnoDB') === true, mysqli_error($db));

$create = "CREATE TABLE wp_options (
    option_id bigint unsigned NOT NULL AUTO_INCREMENT,
    option_name varchar(191) NOT NULL,
    option_value longtext NOT NULL,
    autoload varchar(20) NOT NULL,
    PRIMARY KEY (option_id),
    UNIQUE KEY option_name (option_name)
) ENGINE=InnoDB";
assert_true(mysqli_query($db, $create) === true, mysqli_error($db));

$stmt = mysqli_prepare(
    $db,
    'INSERT INTO wp_options (option_name, option_value, autoload) VALUES (?, ?, ?)'
);
assert_true($stmt instanceof mysqli_stmt, mysqli_error($db));
$name = 'siteurl';
$value = 'https://example.test';
$autoload = 'yes';
assert_true(mysqli_stmt_bind_param($stmt, 'sss', $name, $value, $autoload), mysqli_stmt_error($stmt));
assert_true(mysqli_stmt_execute($stmt), mysqli_stmt_error($stmt));
assert_true(mysqli_stmt_affected_rows($stmt) === 1, 'unexpected insert affected rows');
assert_true(mysqli_stmt_insert_id($stmt) === 1, 'unexpected insert id');
mysqli_stmt_close($stmt);

$escaped = mysqli_real_escape_string($db, "Jan's Blog");
assert_true($escaped === "Jan\\'s Blog", 'escape result mismatch');

$stmt = mysqli_prepare($db, 'SELECT option_value FROM wp_options WHERE option_name = ?');
assert_true($stmt instanceof mysqli_stmt, mysqli_error($db));
assert_true(mysqli_stmt_bind_param($stmt, 's', $name), mysqli_stmt_error($stmt));
assert_true(mysqli_stmt_execute($stmt), mysqli_stmt_error($stmt));
$result = mysqli_stmt_get_result($stmt);
assert_true($result instanceof mysqli_result, mysqli_stmt_error($stmt));
$row = mysqli_fetch_assoc($result);
assert_true(is_array($row), 'fetch_assoc did not return a row');
assert_true($row['option_value'] === 'https://example.test', 'fetched option value mismatch');
mysqli_free_result($result);
mysqli_stmt_close($stmt);

$result = $db->query('SELECT option_id, option_name FROM wp_options');
assert_true($result instanceof mysqli_result, $db->error);
$rows = $result->fetch_all(MYSQLI_ASSOC);
assert_true(count($rows) === 1, 'fetch_all row count mismatch');
assert_true($rows[0]['option_name'] === 'siteurl', 'fetch_all value mismatch');
$result->free();

assert_true(mysqli_close($db), 'close failed');
@unlink($path);
