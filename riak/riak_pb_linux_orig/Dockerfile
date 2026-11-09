FROM php:8.4.27-cli-bookworm AS build

WORKDIR /build/riak_pb
COPY . .
RUN phpize \
    && ./configure --enable-riak-pb CFLAGS="-O2 -Wall -Wextra -Werror" \
    && make -j2 \
    && cc -std=c11 -Wall -Wextra -Werror -pedantic \
        pb_codec.c tests/codec_test.c -o /tmp/riak-codec-test \
    && /tmp/riak-codec-test

FROM build AS test
RUN sed -i 's|http://deb.debian.org|https://deb.debian.org|g' /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=20 update \
    && apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=20 install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
RUN python3 tests/mock_server.py --php php --extension modules/riak_pb.so

CMD ["php", "-n", "-d", "extension=/build/riak_pb/modules/riak_pb.so", "--ri", "riak_pb"]
