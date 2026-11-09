<?php
declare(strict_types=1);

$functions = [
    'riak_pb_ping', 'riak_pb_server_info', 'riak_pb_get',
    'riak_pb_put', 'riak_pb_delete', 'riak_pb_set_bucket_allow_mult',
];
$available = [];
foreach ($functions as $function) {
    $available[$function] = function_exists($function);
}
$result = [
    'php_version' => PHP_VERSION,
    'sapi' => PHP_SAPI,
    'zts' => PHP_ZTS,
    'debug' => PHP_DEBUG,
    'bits' => PHP_INT_SIZE * 8,
    'extension_dir' => ini_get('extension_dir'),
    'riak_pb_loaded' => extension_loaded('riak_pb'),
    'riak_pb_version' => phpversion('riak_pb'),
    'functions' => $available,
];
if (!extension_loaded('riak_pb') || in_array(false, $available, true)) {
    if (PHP_SAPI !== 'cli') {
        http_response_code(500);
    }
    echo json_encode($result, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
    exit(1);
}
try {
    riak_pb_ping('127.0.0.1', 0);
    throw new RuntimeException('Expected invalid port to be rejected before networking');
} catch (ValueError $error) {
    $result['native_parameter_validation'] = 'PASS';
}
if (PHP_SAPI !== 'cli') {
    header('Content-Type: application/json');
}
echo json_encode($result, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), "\n";
