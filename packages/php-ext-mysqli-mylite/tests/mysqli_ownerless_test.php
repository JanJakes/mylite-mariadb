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

function ownerless_option(): int
{
    return constant('MyLite\\MYSQLI_OPT_OWNERLESS_RW');
}

function open_ownerless_mysqli(string $path): MyLite\MySQLi
{
    $db = MyLite\mysqli_init();
    expect_true($db instanceof MyLite\MySQLi, 'MyLite mysqli_init returned the wrong object');
    expect_true(
        MyLite\mysqli_options($db, ownerless_option(), true),
        'MyLite mysqli ownerless option was rejected'
    );
    expect_true(
        MyLite\mysqli_real_connect($db, $path),
        'MyLite ownerless real_connect failed: ' . $db->error
    );
    return $db;
}

function open_ordinary_mysqli(string $path): MyLite\MySQLi
{
    $db = MyLite\mysqli_init();
    expect_true($db instanceof MyLite\MySQLi, 'ordinary MyLite mysqli_init returned the wrong object');
    expect_true(
        MyLite\mysqli_real_connect($db, $path),
        'ordinary MyLite real_connect failed: ' . $db->error
    );
    return $db;
}

function child_command(string $path, string $mode): array
{
    $coreExtension = getenv('MYLITE_PHP_CORE_EXTENSION');
    $mysqliExtension = getenv('MYLITE_PHP_MYSQLI_EXTENSION');
    expect_true(is_string($coreExtension) && $coreExtension !== '', 'missing core extension path');
    expect_true(
        is_string($mysqliExtension) && $mysqliExtension !== '',
        'missing mysqli extension path'
    );

    return [
        PHP_BINARY,
        '-d',
        'extension=' . $coreExtension,
        '-d',
        'extension=' . $mysqliExtension,
        __FILE__,
        $mode,
        $path,
    ];
}

function start_child(string $path, string $mode = '--child'): array
{
    $pipes = [];
    $process = proc_open(
        child_command($path, $mode),
        [
            0 => ['pipe', 'r'],
            1 => ['pipe', 'w'],
            2 => ['pipe', 'w'],
        ],
        $pipes
    );
    expect_true(is_resource($process), 'could not start ownerless mysqli child');
    return ['process' => $process, 'pipes' => $pipes];
}

function send_child_command(array $child, string $command): void
{
    expect_true(
        fwrite($child['pipes'][0], $command . PHP_EOL) !== false,
        "could not send '{$command}' to ownerless mysqli child"
    );
    expect_true(fflush($child['pipes'][0]), 'could not flush ownerless mysqli child command');
}

function expect_child_signal(array $child, string $expected): void
{
    $deadline = microtime(true) + 20.0;
    while (microtime(true) < $deadline) {
        $remaining = $deadline - microtime(true);
        $seconds = (int)$remaining;
        $microseconds = (int)(($remaining - $seconds) * 1000000);
        $read = [$child['pipes'][1]];
        $write = null;
        $except = null;
        $ready = stream_select($read, $write, $except, $seconds, $microseconds);
        expect_true($ready !== false, 'could not wait for ownerless mysqli child signal');
        if ($ready === 0) {
            break;
        }

        $line = fgets($child['pipes'][1]);
        if ($line === false) {
            if (feof($child['pipes'][1])) {
                break;
            }
            continue;
        }
        $actual = rtrim($line, "\r\n");
        expect_true(
            $actual === $expected,
            "ownerless mysqli child signal mismatch: expected '{$expected}', got '{$actual}'"
        );
        return;
    }

    throw new RuntimeException("timed out waiting for ownerless mysqli child signal '{$expected}'");
}

function finish_child(array $child, bool $terminate): array
{
    if ($terminate) {
        proc_terminate($child['process']);
    }
    fclose($child['pipes'][0]);
    $stdout = stream_get_contents($child['pipes'][1]);
    $stderr = stream_get_contents($child['pipes'][2]);
    fclose($child['pipes'][1]);
    fclose($child['pipes'][2]);
    return [proc_close($child['process']), $stdout, $stderr];
}

function child_signal(string $signal): void
{
    expect_true(fwrite(STDOUT, $signal . PHP_EOL) !== false, 'could not signal mysqli parent');
    expect_true(fflush(STDOUT), 'could not flush mysqli parent signal');
}

function expect_child_command(string $expected): void
{
    $line = fgets(STDIN);
    expect_true($line !== false, "mysqli parent closed before '{$expected}' command");
    $actual = rtrim($line, "\r\n");
    expect_true(
        $actual === $expected,
        "ownerless mysqli parent command mismatch: expected '{$expected}', got '{$actual}'"
    );
}

function run_ownerless_child(string $path): void
{
    $db = open_ownerless_mysqli($path);
    expect_true($db->query('USE app') === true, 'child ownerless USE failed: ' . $db->error);
    child_signal('connected');
    expect_true(
        $db->query('START TRANSACTION') === true,
        'child ownerless START TRANSACTION failed: ' . $db->error
    );
    child_signal('transaction-started');
    $stmt = $db->prepare(
        'INSERT INTO events (id, source, payload) VALUES (?, ?, ?)'
    );
    expect_true(
        $stmt instanceof MyLite\MySQLiStmt,
        "child ownerless prepared INSERT failed: {$db->errno} {$db->error}"
    );
    $id = 2;
    $source = 'child-pending';
    $payload = "child\0prepared";
    expect_true(
        $stmt->bind_param('iss', $id, $source, $payload),
        "child ownerless bind_param failed: {$db->errno} {$db->error}"
    );
    expect_true(
        $stmt->execute(),
        "child ownerless prepared INSERT execute failed: {$db->errno} {$db->error}"
    );
    unset($stmt);
    child_signal('transaction-open');

    expect_child_command('commit');
    expect_true($db->query('COMMIT') === true, 'child ownerless COMMIT failed: ' . $db->error);
    child_signal('transaction-committed');

    expect_child_command('contend');
    expect_true(
        $db->query('SET SESSION innodb_lock_wait_timeout = 1') === true,
        'child ownerless lock timeout setup failed: ' . $db->error
    );
    $lockResult = $db->query("UPDATE events SET source = 'child-timeout' WHERE id = 1");
    expect_true($lockResult === false, 'conflicting ownerless mysqli UPDATE did not time out');
    expect_true(
        $db->errno === 1205,
        "conflicting ownerless mysqli UPDATE errno mismatch: {$db->errno} {$db->error}"
    );
    expect_true(
        $db->sqlstate === 'HY000' && $db->error !== '',
        "conflicting ownerless mysqli UPDATE diagnostics mismatch: {$db->sqlstate} {$db->error}"
    );
    child_signal('lock-timeout');

    expect_child_command('alter-schema');
    expect_true(
        $db->query(
            "ALTER TABLE events ADD COLUMN note VARCHAR(32) NOT NULL DEFAULT 'peer-ddl'"
        ) === true,
        "child ownerless ALTER TABLE failed after lock timeout: {$db->errno} {$db->error}"
    );
    expect_true(
        $db->query('CREATE INDEX source_idx ON events(source)') === true,
        "child ownerless CREATE INDEX failed after ALTER: {$db->errno} {$db->error}"
    );
    child_signal('schema-altered');

    expect_child_command('close');
    expect_true($db->close(), 'child ownerless mysqli close failed');
    child_signal('closed');
}

function run_ownerless_post_timeout_child(string $path): void
{
    $db = open_ownerless_mysqli($path);
    expect_true(
        $db->query('USE app') === true,
        'post-timeout child ownerless USE failed: ' . $db->error
    );
    $stmt = $db->prepare(
        'INSERT INTO events (id, source, payload, note) VALUES (?, ?, ?, ?)'
    );
    expect_true(
        $stmt instanceof MyLite\MySQLiStmt,
        "post-timeout child prepared INSERT failed: {$db->errno} {$db->error}"
    );
    $id = 3;
    $source = 'child-after';
    $payload = "after\0one";
    $note = 'bound-first';
    expect_true(
        $stmt->bind_param('isss', $id, $source, $payload, $note),
        "post-timeout child bind_param failed: {$db->errno} {$db->error}"
    );
    expect_true(
        $stmt->execute(),
        "post-timeout child first prepared INSERT failed: {$db->errno} {$db->error}"
    );
    $id = 4;
    $source = 'child-rebound';
    $payload = "after\0two";
    $note = 'bound-second';
    expect_true(
        $stmt->execute(),
        "post-timeout child rebound prepared INSERT failed: {$db->errno} {$db->error}"
    );
    unset($stmt);
    child_signal('post-timeout-writes');
    expect_true($db->close(), 'post-timeout child ownerless mysqli close failed');
    child_signal('closed');
}

if (in_array(($argv[1] ?? ''), ['--child', '--post-timeout-child'], true)) {
    try {
        if ($argv[1] === '--post-timeout-child') {
            run_ownerless_post_timeout_child($argv[2]);
        } else {
            run_ownerless_child($argv[2]);
        }
        exit(0);
    } catch (Throwable $exception) {
        fwrite(STDERR, $exception->getMessage() . PHP_EOL);
        exit(2);
    }
}

if (PHP_OS_FAMILY !== 'Linux') {
    fwrite(STDOUT, "ownerless mysqli multiprocess test skipped outside Linux\n");
    exit(0);
}

expect_true(extension_loaded('mylite'), 'mylite extension is not loaded');
expect_true(extension_loaded('mysqli_mylite'), 'mysqli_mylite extension is not loaded');
expect_true(defined('MyLite\\MYSQLI_OPT_OWNERLESS_RW'), 'ownerless mysqli option is missing');

$path = sys_get_temp_dir() . '/mylite-php-mysqli-ownerless-' . getmypid() . '.mylite';
remove_tree($path);
register_shutdown_function('remove_tree', $path);

$db = open_ownerless_mysqli($path);
expect_true($db->query('CREATE DATABASE app'), 'ownerless CREATE DATABASE failed');
expect_true($db->query('USE app'), 'ownerless USE failed');
expect_true(
    $db->query(
        'CREATE TABLE events (' .
        'id INT PRIMARY KEY, source VARCHAR(32), payload VARBINARY(64)' .
        ') ENGINE=InnoDB'
    ),
    'ownerless CREATE TABLE failed'
);
expect_true(
    $db->query("INSERT INTO events VALUES (1, 'parent', X'706172656E74')"),
    'parent INSERT failed'
);
expect_true(
    !$db->options(ownerless_option(), false),
    'ownerless option changed an already-connected mysqli handle'
);

$child = start_child($path);
try {
    expect_child_signal($child, 'connected');
    expect_child_signal($child, 'transaction-started');
    expect_child_signal($child, 'transaction-open');
    $result = $db->query('SELECT COUNT(*) AS total FROM events WHERE id = 2');
    expect_true(
        $result instanceof MyLite\MySQLiResult,
        "uncommitted visibility SELECT failed: {$db->errno} {$db->error}"
    );
    expect_true(
        $result->fetch_assoc() === ['total' => '0'],
        'parent observed the child mysqli transaction before commit'
    );

    send_child_command($child, 'commit');
    expect_child_signal($child, 'transaction-committed');
    $result = $db->query('SELECT source, HEX(payload) AS payload_hex FROM events WHERE id = 2');
    expect_true(
        $result instanceof MyLite\MySQLiResult,
        "committed visibility SELECT failed: {$db->errno} {$db->error}"
    );
    expect_true(
        $result->fetch_assoc() === [
            'source' => 'child-pending',
            'payload_hex' => '6368696C64007072657061726564',
        ],
        'parent did not observe the committed prepared child mysqli transaction'
    );

    expect_true($db->query('START TRANSACTION') === true, 'parent START TRANSACTION failed');
    expect_true(
        $db->query("UPDATE events SET source = 'parent-lock' WHERE id = 1") === true,
        'parent locking UPDATE failed: ' . $db->error
    );
    send_child_command($child, 'contend');
    expect_child_signal($child, 'lock-timeout');
    $result = $db->query('SELECT source FROM events WHERE id = 1');
    expect_true($result instanceof MyLite\MySQLiResult, 'parent transaction SELECT failed');
    expect_true(
        $result->fetch_assoc() === ['source' => 'parent-lock'],
        'parent transaction lost its locked row value'
    );
    expect_true($db->query('COMMIT') === true, 'parent COMMIT failed: ' . $db->error);

    send_child_command($child, 'alter-schema');
    expect_child_signal($child, 'schema-altered');
    $result = $db->query(
        "SELECT COLUMN_DEFAULT FROM information_schema.columns " .
        "WHERE table_schema = 'app' AND table_name = 'events' AND column_name = 'note'"
    );
    expect_true(
        $result instanceof MyLite\MySQLiResult,
        "peer DDL column refresh SELECT failed: {$db->errno} {$db->error}"
    );
    expect_true(
        $result->fetch_assoc() === ['COLUMN_DEFAULT' => "'peer-ddl'"],
        'already-open mysqli peer did not refresh the new column metadata'
    );
    $result = $db->query(
        "SELECT COUNT(*) AS total FROM information_schema.statistics " .
        "WHERE table_schema = 'app' AND table_name = 'events' AND index_name = 'source_idx'"
    );
    expect_true(
        $result instanceof MyLite\MySQLiResult && $result->fetch_assoc() === ['total' => '1'],
        'already-open mysqli peer did not refresh the new index metadata'
    );
    $result = $db->query('SELECT note FROM events WHERE id = 1');
    expect_true(
        $result instanceof MyLite\MySQLiResult &&
            $result->fetch_assoc() === ['note' => 'peer-ddl'],
        'already-open mysqli peer did not observe the peer DDL default'
    );

    send_child_command($child, 'close');
    expect_child_signal($child, 'closed');
} catch (Throwable $exception) {
    [$status, $stdout, $stderr] = finish_child($child, true);
    throw new RuntimeException(
        $exception->getMessage() .
        " (terminated child status {$status}; remaining stdout: {$stdout}; stderr: {$stderr})",
        0,
        $exception
    );
}

[$status, $stdout, $stderr] = finish_child($child, false);
expect_true(
    $status === 0,
    "ownerless mysqli child failed with status {$status}: {$stdout}{$stderr}"
);

$postTimeoutChild = start_child($path, '--post-timeout-child');
try {
    expect_child_signal($postTimeoutChild, 'post-timeout-writes');
    expect_child_signal($postTimeoutChild, 'closed');
} catch (Throwable $exception) {
    [$status, $stdout, $stderr] = finish_child($postTimeoutChild, true);
    throw new RuntimeException(
        $exception->getMessage() .
        " (terminated post-timeout child status {$status}; remaining stdout: {$stdout}; stderr: {$stderr})",
        0,
        $exception
    );
}
[$status, $stdout, $stderr] = finish_child($postTimeoutChild, false);
expect_true(
    $status === 0,
    "ownerless mysqli post-timeout child failed with status {$status}: {$stdout}{$stderr}"
);

$result = $db->query(
    'SELECT id, source, HEX(payload) AS payload_hex, note FROM events ORDER BY id'
);
expect_true($result instanceof MyLite\MySQLiResult, 'ownerless SELECT returned no result');
$observedRows = $result->fetch_all(MYSQLI_ASSOC);
expect_true(
    $observedRows === [
        [
            'id' => '1',
            'source' => 'parent-lock',
            'payload_hex' => '706172656E74',
            'note' => 'peer-ddl',
        ],
        [
            'id' => '2',
            'source' => 'child-pending',
            'payload_hex' => '6368696C64007072657061726564',
            'note' => 'peer-ddl',
        ],
        [
            'id' => '3',
            'source' => 'child-after',
            'payload_hex' => '6166746572006F6E65',
            'note' => 'bound-first',
        ],
        [
            'id' => '4',
            'source' => 'child-rebound',
            'payload_hex' => '61667465720074776F',
            'note' => 'bound-second',
        ],
    ],
    'cross-process mysqli transaction and lock result mismatch: ' .
        json_encode($observedRows, JSON_UNESCAPED_SLASHES)
);

$stmt = $db->prepare(
    'INSERT INTO events (id, source, payload, note) VALUES (?, ?, ?, ?)'
);
$preparedId = 5;
$preparedSource = 'prepared';
$preparedPayload = "parent\0bound";
$preparedNote = 'parent-prepared';
expect_true($stmt instanceof MyLite\MySQLiStmt, 'ownerless prepared INSERT failed');
expect_true(
    $stmt->bind_param(
        'isss',
        $preparedId,
        $preparedSource,
        $preparedPayload,
        $preparedNote
    ),
    'ownerless bind_param failed'
);
expect_true($stmt->execute(), 'ownerless prepared INSERT execute failed');
$stmt = $db->prepare('SELECT source, HEX(payload) AS payload_hex, note FROM events WHERE id = ?');
expect_true($stmt instanceof MyLite\MySQLiStmt, 'ownerless prepared SELECT failed');
expect_true($stmt->bind_param('i', $preparedId), 'ownerless SELECT bind_param failed');
expect_true($stmt->execute(), 'ownerless prepared SELECT execute failed');
expect_true(
    $stmt->get_result()->fetch_assoc() === [
        'source' => 'prepared',
        'payload_hex' => '706172656E7400626F756E64',
        'note' => 'parent-prepared',
    ],
    'ownerless prepared SELECT result mismatch'
);

expect_true(
    $db->query('CREATE TABLE unsupported_engine (id INT PRIMARY KEY) ENGINE=MyISAM') === false,
    'ownerless mysqli accepted MyISAM DDL'
);
expect_true(
    $db->errno !== 0 && $db->sqlstate === 'HY000' && $db->error !== '',
    'MyISAM policy diagnostics mismatch'
);
$result = $db->query(
    "SELECT COUNT(*) AS total FROM information_schema.tables " .
    "WHERE table_schema = 'app' AND table_name = 'unsupported_engine'"
);
expect_true(
    $result instanceof MyLite\MySQLiResult && $result->fetch_assoc() === ['total' => '0'],
    'rejected MyISAM DDL left a table behind'
);
expect_true(
    $db->query('CREATE FULLTEXT INDEX unsupported_fulltext ON events(source)') === false,
    'ownerless mysqli accepted FULLTEXT DDL'
);
expect_true(
    $db->errno !== 0 && $db->sqlstate === 'HY000' && $db->error !== '',
    'FULLTEXT policy diagnostics mismatch'
);
$result = $db->query(
    "SELECT COUNT(*) AS total FROM information_schema.statistics " .
    "WHERE table_schema = 'app' AND table_name = 'events' " .
    "AND index_name = 'unsupported_fulltext'"
);
expect_true(
    $result instanceof MyLite\MySQLiResult && $result->fetch_assoc() === ['total' => '0'],
    'rejected FULLTEXT DDL left an index behind'
);
unset($stmt, $result);
expect_true(
    $db->close(),
    "ownerless mysqli close failed: {$db->errno} {$db->sqlstate} {$db->error}"
);

$db = open_ordinary_mysqli($path);
expect_true($db->query('USE app') === true, 'ordinary mysqli reopen USE failed');
$result = $db->query(
    'SELECT id, source, HEX(payload) AS payload_hex, note FROM events ORDER BY id'
);
expect_true($result instanceof MyLite\MySQLiResult, 'ordinary mysqli reopen SELECT failed');
expect_true(
    $result->fetch_all(MYSQLI_ASSOC) === [
        [
            'id' => '1',
            'source' => 'parent-lock',
            'payload_hex' => '706172656E74',
            'note' => 'peer-ddl',
        ],
        [
            'id' => '2',
            'source' => 'child-pending',
            'payload_hex' => '6368696C64007072657061726564',
            'note' => 'peer-ddl',
        ],
        [
            'id' => '3',
            'source' => 'child-after',
            'payload_hex' => '6166746572006F6E65',
            'note' => 'bound-first',
        ],
        [
            'id' => '4',
            'source' => 'child-rebound',
            'payload_hex' => '61667465720074776F',
            'note' => 'bound-second',
        ],
        [
            'id' => '5',
            'source' => 'prepared',
            'payload_hex' => '706172656E7400626F756E64',
            'note' => 'parent-prepared',
        ],
    ],
    'ordinary mysqli reopen state mismatch'
);
expect_true($db->close(), 'ordinary mysqli reopen close failed');
