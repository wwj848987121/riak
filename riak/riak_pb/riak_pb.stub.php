<?php
/**
 * IDE / static-analysis stub for the native riak_pb extension.
 *
 * Never include or require this file: the functions are provided by
 * php_riak_pb.dll / riak_pb.so at runtime, and loading this stub would
 * redeclare them. Signatures mirror the arginfo in riak_pb.c.
 *
 * All network or server failures throw \RuntimeException (server errors
 * carry the Riak error code as the exception code); invalid arguments
 * throw \ValueError before any connection is made.
 *
 * @generate-function-entries false
 */

/**
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_ping(string $host, int $port = 8129, int $timeout_ms = 3000): bool {}

/**
 * @return array{node: string, server_version: string}
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_server_info(string $host, int $port = 8129, int $timeout_ms = 3000): array {}

/**
 * @return array{content: string, content_type: string}|null null when the key does not exist
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_get(string $host, int $port, string $bucket, string $key, int $timeout_ms = 3000): ?array {}

/**
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_put(string $host, int $port, string $bucket, string $key, string $content, string $content_type = "application/octet-stream", int $timeout_ms = 3000): bool {}

/**
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_delete(string $host, int $port, string $bucket, string $key, int $timeout_ms = 3000): bool {}

/**
 * @throws RuntimeException
 * @throws ValueError
 */
function riak_pb_set_bucket_allow_mult(string $host, int $port, string $bucket, bool $allow_mult, int $timeout_ms = 3000): bool {}
