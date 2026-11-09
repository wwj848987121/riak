<?php
// Invoked by mock_server.py. No request is sent to a real Riak server.
declare(strict_types=1);

if (PHP_SAPI !== 'cli' || !extension_loaded('riak_pb')) {
    fwrite(STDERR, "Run via mock_server.py with a compiled riak_pb extension.\n");
    exit(2);
}

$host = '127.0.0.1';
$port = filter_var($argv[1] ?? '', FILTER_VALIDATE_INT);
if ($port === false || $port < 1 || $port > 65535) {
    fwrite(STDERR, "A local mock-server port is required.\n");
    exit(2);
}

function checkSame($expected, $actual, string $label): void
{
    if ($expected !== $actual) {
        throw new RuntimeException($label . ': unexpected result');
    }
    echo "PASS: {$label}\n";
}

function checkThrows(callable $operation, string $label, string $message = ''): void
{
    try {
        $operation();
    } catch (Throwable $error) {
        if ($message !== '' && strpos($error->getMessage(), $message) === false) {
            throw new RuntimeException($label . ': incorrect error: ' . $error->getMessage());
        }
        echo "PASS: {$label}\n";
        return;
    }
    throw new RuntimeException($label . ': expected an exception');
}

function checkValueError(callable $operation, string $label): void
{
    try {
        $operation();
    } catch (ValueError $error) {
        echo "PASS: {$label}\n";
        return;
    }
    throw new RuntimeException($label . ': expected parameter validation before networking');
}

try {
    checkSame(true, riak_pb_ping($host, $port), 'ping with fragmented frame');
    checkSame(['node' => 'riak@mock', 'server_version' => '2.0.0'],
        riak_pb_server_info($host, $port), 'server version');
    checkSame(true, riak_pb_set_bucket_allow_mult($host, $port, 'test', false),
        'allow_mult=false protobuf field');

    $key = "file.with.dots/测试\0.bin";
    $binary = str_repeat(implode('', array_map('chr', range(0, 255))), 512);
    checkSame(true, riak_pb_put($host, $port, 'test', $key, $binary, 'application/octet-stream'),
        'binary upload with explicit key');
    checkSame(['content' => $binary, 'content_type' => 'application/octet-stream'],
        riak_pb_get($host, $port, 'test', $key), 'binary round trip');
    checkSame(true, riak_pb_put($host, $port, 'test', 'empty', '', 'text/plain'), 'empty upload');
    checkSame(['content' => '', 'content_type' => 'text/plain'],
        riak_pb_get($host, $port, 'test', 'empty'), 'empty file is present');
    checkSame(true, riak_pb_put($host, $port, 'test', 'default-upload-mime', 'data'),
        'omitted MIME upload');
    checkSame(['content' => 'data', 'content_type' => 'application/octet-stream'],
        riak_pb_get($host, $port, 'test', 'default-upload-mime'), 'default MIME round trip');
    checkSame(null, riak_pb_get($host, $port, 'test', 'missing'), 'missing object');
    checkSame(null, riak_pb_get($host, $port, 'test', 'deleted'), 'deleted tombstone');
    checkSame(['content' => 'known', 'content_type' => 'text/plain'],
        riak_pb_get($host, $port, 'test', 'unknown-fields'), 'unknown protobuf fields');
    checkSame(['content' => 'raw', 'content_type' => 'application/octet-stream'],
        riak_pb_get($host, $port, 'test', 'default-mime'), 'missing MIME fallback');
    checkSame(true, riak_pb_delete($host, $port, 'test', $key), 'delete');
    checkSame(null, riak_pb_get($host, $port, 'test', $key), 'read after delete');
    checkSame(true, riak_pb_delete($host, $port, 'test', $key), 'delete absent object');

    checkThrows(fn() => riak_pb_get($host, $port, 'test', 'server-error'),
        'server error propagation', 'bucket unavailable');
    foreach (['error-without-message', 'error-without-code', 'error-code-overflow',
              'error-wrong-wire', 'error-truncated'] as $scenario) {
        checkThrows(fn() => riak_pb_get($host, $port, 'test', $scenario),
            $scenario, 'Malformed');
    }
    foreach (['wrong-code', 'malformed', 'malformed-content', 'missing-value',
              'wrong-value-wire', 'oversize', 'zero-frame', 'closed',
              'truncated-frame', 'siblings'] as $scenario) {
        checkThrows(fn() => riak_pb_get($host, $port, 'test', $scenario), $scenario);
    }
    $started = microtime(true);
    checkThrows(fn() => riak_pb_get($host, $port, 'test', 'timeout', 100), 'response timeout');
    if (microtime(true) - $started > 1.0) {
        throw new RuntimeException('response timeout exceeded one second');
    }
    $started = microtime(true);
    checkThrows(fn() => riak_pb_get($host, $port, 'test', 'slow-drip', 120),
        'absolute deadline despite continuously arriving bytes', 'timed out');
    if (microtime(true) - $started > 0.75) {
        throw new RuntimeException('fragmented response reset the operation deadline');
    }

    foreach (['', "127.0.0.1\0other", 'tcp://127.0.0.1', 'user@127.0.0.1',
              "127.0.0.1\r\n", str_repeat('a', 254)] as $invalidHost) {
        checkValueError(fn() => riak_pb_ping($invalidHost, $port), 'invalid host validation');
    }
    foreach ([-1, 0, 65536, PHP_INT_MAX] as $invalidPort) {
        checkValueError(fn() => riak_pb_ping($host, $invalidPort), 'invalid port validation');
    }
    foreach ([-1, 0, 600001, PHP_INT_MAX] as $invalidTimeout) {
        checkValueError(fn() => riak_pb_ping($host, $port, $invalidTimeout),
            'invalid timeout validation');
    }
    foreach (['', 'plain', '/plain', 'text/', 'text/plain/extra',
              "text/plain\0", "text/plain\r\nInjected: x", 'text/plain; charset=utf-8',
              str_repeat('a', 255) . '/x'] as $invalidMime) {
        checkValueError(fn() => riak_pb_put($host, $port, 'test', 'invalid-mime', 'data', $invalidMime),
            'invalid MIME validation');
    }
    checkValueError(fn() => riak_pb_get($host, $port, '', 'key'), 'empty bucket validation');
    checkValueError(fn() => riak_pb_get($host, $port, 'test', ''), 'empty key validation');
    checkSame(true, riak_pb_ping($host, $port, 600000), 'maximum allowed timeout');
    checkSame(true, riak_pb_ping($host, $port), 'healthy request after errors');
    echo "All offline Riak PB integration checks passed.\n";
} catch (Throwable $error) {
    fwrite(STDERR, 'FAIL: ' . $error->getMessage() . "\n");
    exit(1);
}
