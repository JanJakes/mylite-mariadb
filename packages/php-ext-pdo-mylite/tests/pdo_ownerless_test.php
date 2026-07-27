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

function ownerless_dsn(string $path): string
{
    return 'mylite:path=' . $path . ';mode=ownerless_rw';
}

function child_command(string $path, string $mode): array
{
    $coreExtension = getenv('MYLITE_PHP_CORE_EXTENSION');
    $pdoExtension = getenv('MYLITE_PHP_PDO_EXTENSION');
    expect_true(is_string($coreExtension) && $coreExtension !== '', 'missing core extension path');
    expect_true(is_string($pdoExtension) && $pdoExtension !== '', 'missing PDO extension path');

    return [
        PHP_BINARY,
        '-d',
        'extension=' . $coreExtension,
        '-d',
        'extension=' . $pdoExtension,
        __FILE__,
        $mode,
        $path,
    ];
}

function start_child(string $path): array
{
    $pipes = [];
    $process = proc_open(
        child_command($path, '--child'),
        [
            0 => ['pipe', 'r'],
            1 => ['pipe', 'w'],
            2 => ['pipe', 'w'],
        ],
        $pipes
    );
    expect_true(is_resource($process), 'could not start ownerless PDO child');
    return ['process' => $process, 'pipes' => $pipes];
}

function send_child_command(array $child, string $command): void
{
    expect_true(
        fwrite($child['pipes'][0], $command . PHP_EOL) !== false,
        "could not send '{$command}' to ownerless PDO child"
    );
    expect_true(fflush($child['pipes'][0]), 'could not flush ownerless PDO child command');
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
        expect_true($ready !== false, 'could not wait for ownerless PDO child signal');
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
            "ownerless PDO child signal mismatch: expected '{$expected}', got '{$actual}'"
        );
        return;
    }

    throw new RuntimeException("timed out waiting for ownerless PDO child signal '{$expected}'");
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
    expect_true(fwrite(STDOUT, $signal . PHP_EOL) !== false, 'could not signal PDO parent');
    expect_true(fflush(STDOUT), 'could not flush PDO parent signal');
}

function expect_child_command(string $expected): void
{
    $line = fgets(STDIN);
    expect_true($line !== false, "PDO parent closed before '{$expected}' command");
    $actual = rtrim($line, "\r\n");
    expect_true(
        $actual === $expected,
        "ownerless PDO parent command mismatch: expected '{$expected}', got '{$actual}'"
    );
}

function open_ownerless_pdo(string $path): PDO
{
    return new PDO(ownerless_dsn($path), null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    ]);
}

function open_ordinary_pdo(string $path): PDO
{
    return new PDO('mylite:path=' . $path, null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    ]);
}

function run_ownerless_child(string $path): void
{
    $pdo = open_ownerless_pdo($path);
    $pdo->exec('USE app');
    child_signal('connected');
    expect_true($pdo->beginTransaction(), 'child ownerless PDO beginTransaction failed');
    child_signal('transaction-started');
    $stmt = $pdo->prepare(
        'INSERT INTO events (id, source, payload) VALUES (?, ?, ?)'
    );
    expect_true($stmt !== false, 'child ownerless PDO prepare failed');
    expect_true($stmt->bindValue(1, 2, PDO::PARAM_INT), 'child PDO integer binding failed');
    expect_true(
        $stmt->bindValue(2, 'child-pending', PDO::PARAM_STR),
        'child PDO source binding failed'
    );
    expect_true(
        $stmt->bindValue(3, "child\0prepared", PDO::PARAM_STR),
        'child PDO binary binding failed'
    );
    expect_true(
        $stmt->execute(),
        'child ownerless PDO prepared transactional INSERT failed: ' .
            json_encode($stmt->errorInfo(), JSON_UNESCAPED_SLASHES)
    );
    unset($stmt);
    child_signal('transaction-open');

    expect_child_command('commit');
    expect_true($pdo->commit(), 'child ownerless PDO commit failed');
    child_signal('transaction-committed');

    expect_child_command('contend');
    expect_true(
        $pdo->exec('SET SESSION innodb_lock_wait_timeout = 1') !== false,
        'child ownerless PDO lock timeout setup failed'
    );
    try {
        $pdo->exec("UPDATE events SET source = 'child-timeout' WHERE id = 1");
        throw new RuntimeException('conflicting ownerless PDO UPDATE did not time out');
    } catch (PDOException $exception) {
        $nativeCode = (int)($exception->errorInfo[1] ?? 0);
        expect_true(
            $nativeCode === 1205,
            "conflicting ownerless PDO UPDATE errno mismatch: {$nativeCode} {$exception->getMessage()}"
        );
        expect_true(
            ($exception->errorInfo[0] ?? '') === 'HY000' &&
                ($exception->errorInfo[2] ?? '') !== '',
            'conflicting ownerless PDO UPDATE diagnostics mismatch'
        );
    }
    child_signal('lock-timeout');

    expect_child_command('alter-schema');
    expect_true(
        $pdo->exec(
            "ALTER TABLE events ADD COLUMN note VARCHAR(32) NOT NULL DEFAULT 'peer-ddl'"
        ) !== false,
        'child ownerless PDO ALTER TABLE failed after lock timeout'
    );
    expect_true(
        $pdo->exec('CREATE INDEX source_idx ON events(source)') !== false,
        'child ownerless PDO CREATE INDEX failed after ALTER'
    );
    child_signal('schema-altered');

    expect_child_command('close');
    unset($pdo);
    child_signal('closed');
}

if (($argv[1] ?? '') === '--child') {
    try {
        run_ownerless_child($argv[2]);
        exit(0);
    } catch (Throwable $exception) {
        fwrite(STDERR, $exception->getMessage() . PHP_EOL);
        exit(2);
    }
}

if (PHP_OS_FAMILY !== 'Linux') {
    fwrite(STDOUT, "ownerless PDO multiprocess test skipped outside Linux\n");
    exit(0);
}

expect_true(extension_loaded('mylite'), 'mylite extension is not loaded');
expect_true(extension_loaded('pdo_mylite'), 'pdo_mylite extension is not loaded');

$path = sys_get_temp_dir() . '/mylite-php-pdo-ownerless-' . getmypid() . '.mylite';
remove_tree($path);
register_shutdown_function('remove_tree', $path);

$pdo = open_ownerless_pdo($path);
$pdo->exec('CREATE DATABASE app');
$pdo->exec('USE app');
$pdo->exec(
    'CREATE TABLE events (' .
    'id INT PRIMARY KEY, source VARCHAR(32), payload VARBINARY(64)' .
    ') ENGINE=InnoDB'
);
$pdo->exec("INSERT INTO events VALUES (1, 'parent', X'706172656E74')");

$child = start_child($path);
try {
    expect_child_signal($child, 'connected');
    expect_child_signal($child, 'transaction-started');
    expect_child_signal($child, 'transaction-open');
    expect_true(
        (int)$pdo->query('SELECT COUNT(*) FROM events WHERE id = 2')->fetchColumn() === 0,
        'parent observed the child PDO transaction before commit'
    );

    send_child_command($child, 'commit');
    expect_child_signal($child, 'transaction-committed');
    expect_true(
        $pdo->query(
            'SELECT CONCAT(source, ":", HEX(payload)) FROM events WHERE id = 2'
        )->fetchColumn() === 'child-pending:6368696C64007072657061726564',
        'parent did not observe the committed prepared child PDO transaction'
    );

    expect_true($pdo->beginTransaction(), 'parent PDO beginTransaction failed');
    expect_true(
        $pdo->exec("UPDATE events SET source = 'parent-lock' WHERE id = 1") === 1,
        'parent PDO locking UPDATE failed'
    );
    send_child_command($child, 'contend');
    expect_child_signal($child, 'lock-timeout');
    expect_true(
        $pdo->query('SELECT source FROM events WHERE id = 1')->fetchColumn() === 'parent-lock',
        'parent PDO transaction lost its locked row value'
    );
    expect_true($pdo->commit(), 'parent PDO commit failed');

    send_child_command($child, 'alter-schema');
    expect_child_signal($child, 'schema-altered');
    expect_true(
        $pdo->query(
            "SELECT COLUMN_DEFAULT FROM information_schema.columns " .
            "WHERE table_schema = 'app' AND table_name = 'events' AND column_name = 'note'"
        )->fetchColumn() === "'peer-ddl'",
        'already-open PDO peer did not refresh the new column metadata'
    );
    expect_true(
        (int)$pdo->query(
            "SELECT COUNT(*) FROM information_schema.statistics " .
            "WHERE table_schema = 'app' AND table_name = 'events' AND index_name = 'source_idx'"
        )->fetchColumn() === 1,
        'already-open PDO peer did not refresh the new index metadata'
    );
    expect_true(
        $pdo->query('SELECT note FROM events WHERE id = 1')->fetchColumn() === 'peer-ddl',
        'already-open PDO peer did not observe the peer DDL default'
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
expect_true($status === 0, "ownerless PDO child failed with status {$status}: {$stdout}{$stderr}");

$result = $pdo->query(
    'SELECT id, source, HEX(payload) AS payload_hex, note FROM events ORDER BY id'
);
expect_true($result !== false, 'ownerless PDO SELECT failed');
$observedRows = $result->fetchAll(PDO::FETCH_ASSOC);
expect_true(
    $observedRows === [
        [
            'id' => 1,
            'source' => 'parent-lock',
            'payload_hex' => '706172656E74',
            'note' => 'peer-ddl',
        ],
        [
            'id' => 2,
            'source' => 'child-pending',
            'payload_hex' => '6368696C64007072657061726564',
            'note' => 'peer-ddl',
        ],
    ],
    'cross-process PDO transaction and lock result mismatch: ' .
        json_encode($observedRows, JSON_UNESCAPED_SLASHES)
);

$stmt = $pdo->prepare(
    'INSERT INTO events (id, source, payload, note) VALUES (?, ?, ?, ?)'
);
expect_true(
    $stmt->execute([3, 'prepared', "parent\0bound", 'parent-prepared']),
    'ownerless PDO prepared INSERT failed'
);
$stmt = $pdo->prepare(
    'SELECT source, HEX(payload) AS payload_hex, note FROM events WHERE id = ?'
);
expect_true($stmt->execute([3]), 'ownerless PDO prepared SELECT failed');
expect_true(
    $stmt->fetch(PDO::FETCH_ASSOC) === [
        'source' => 'prepared',
        'payload_hex' => '706172656E7400626F756E64',
        'note' => 'parent-prepared',
    ],
    'ownerless PDO prepared SELECT result mismatch'
);
expect_true($stmt->closeCursor(), 'ownerless PDO prepared SELECT closeCursor failed');
unset($stmt, $result);
try {
    $pdo->exec('CREATE TABLE unsupported_engine (id INT PRIMARY KEY) ENGINE=MyISAM');
    throw new RuntimeException('ownerless PDO accepted MyISAM DDL');
} catch (PDOException $exception) {
    expect_true(
        $exception->getCode() === 'HY000' &&
            (int)($exception->errorInfo[1] ?? 0) !== 0 &&
            ($exception->errorInfo[2] ?? '') !== '',
        'MyISAM PDO diagnostics mismatch'
    );
}
expect_true(
    (int)$pdo->query(
        "SELECT COUNT(*) FROM information_schema.tables " .
        "WHERE table_schema = 'app' AND table_name = 'unsupported_engine'"
    )->fetchColumn() === 0,
    'rejected MyISAM DDL left a table behind'
);
try {
    $pdo->exec('CREATE FULLTEXT INDEX unsupported_fulltext ON events(source)');
    throw new RuntimeException('ownerless PDO accepted FULLTEXT DDL');
} catch (PDOException $exception) {
    expect_true(
        $exception->getCode() === 'HY000' &&
            (int)($exception->errorInfo[1] ?? 0) !== 0 &&
            ($exception->errorInfo[2] ?? '') !== '',
        'FULLTEXT PDO diagnostics mismatch'
    );
}
expect_true(
    (int)$pdo->query(
        "SELECT COUNT(*) FROM information_schema.statistics " .
        "WHERE table_schema = 'app' AND table_name = 'events' " .
        "AND index_name = 'unsupported_fulltext'"
    )->fetchColumn() === 0,
    'rejected FULLTEXT DDL left an index behind'
);
unset($result, $stmt, $pdo);

$pdo = open_ordinary_pdo($path);
$pdo->exec('USE app');
expect_true(
    $pdo->query(
        'SELECT id, source, HEX(payload) AS payload_hex, note FROM events ORDER BY id'
    )->fetchAll(PDO::FETCH_ASSOC) === [
        [
            'id' => 1,
            'source' => 'parent-lock',
            'payload_hex' => '706172656E74',
            'note' => 'peer-ddl',
        ],
        [
            'id' => 2,
            'source' => 'child-pending',
            'payload_hex' => '6368696C64007072657061726564',
            'note' => 'peer-ddl',
        ],
        [
            'id' => 3,
            'source' => 'prepared',
            'payload_hex' => '706172656E7400626F756E64',
            'note' => 'parent-prepared',
        ],
    ],
    'ordinary PDO reopen state mismatch'
);
unset($pdo);

try {
    new PDO('mylite:path=' . $path . ';mode=unsupported');
    throw new RuntimeException('invalid MyLite PDO mode was accepted');
} catch (PDOException $exception) {
    expect_true($exception->getCode() !== '00000', 'invalid PDO mode SQLSTATE mismatch');
}
