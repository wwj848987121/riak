# Offline PB integration tests

After building the extension, run from any working directory:

```sh
python3 extensions/riak_pb/tests/mock_server.py --php php --extension extensions/riak_pb/modules/riak_pb.so
```

The Python standard-library runner creates a TCP server bound only to
`127.0.0.1`, chooses an unused port, runs PHP with `-n` and the specified
extension, then shuts down the server. It does not contact the configured
Riak endpoint. A nonzero exit code indicates a failed PHP assertion, an
incorrect client protobuf request, or a runner failure.

The mock checks outgoing request field numbers and payloads independently
of the C codec. It splits response headers and bodies across writes and
covers binary data including NUL bytes, zero-length files, missing/deleted
objects, ignored unknown protobuf fields, missing Content-Type, ambiguous
siblings, error responses, malformed protobuf, oversized/zero-length frames,
closed/truncated connections, and read timeouts.

The suite also checks that continuously arriving response fragments do not
reset the overall deadline, malformed error responses are rejected, and
invalid host/MIME/port/timeout arguments raise `ValueError` before networking.

Protocol references:

- https://github.com/basho/riak_pb/blob/develop/src/riak_pb_messages.csv
- https://github.com/basho/riak_pb/blob/develop/src/riak.proto
- https://github.com/basho/riak_pb/blob/develop/src/riak_kv.proto

These tests check the basic KV wire contract; they are not a substitute for
the final round-trip check against the actual Riak 2.0.0 deployment.
