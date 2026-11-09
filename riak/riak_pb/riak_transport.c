#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "riak_transport.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>

typedef SOCKET riak_socket;
# define RIAK_INVALID_SOCKET INVALID_SOCKET
# define riak_close_socket closesocket
# define RIAK_ERR_INTR WSAEINTR
# define RIAK_ERR_TIMEDOUT WSAETIMEDOUT
# define RIAK_ERR_CONNREFUSED WSAECONNREFUSED
#else
# include <fcntl.h>
# include <netdb.h>
# include <poll.h>
# include <sys/socket.h>
# include <time.h>
# include <unistd.h>

typedef int riak_socket;
# define RIAK_INVALID_SOCKET (-1)
# define riak_close_socket close
# define RIAK_ERR_INTR EINTR
# define RIAK_ERR_TIMEDOUT ETIMEDOUT
# define RIAK_ERR_CONNREFUSED ECONNREFUSED
#endif

typedef enum { RIAK_WAIT_READ, RIAK_WAIT_WRITE } riak_wait;

static int64_t monotonic_ms(void)
{
#ifdef _WIN32
    return (int64_t) GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}

static int last_socket_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool is_would_block(int code)
{
#ifdef _WIN32
    return code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

/* Formats an errno / WSA error code as text. */
static const char *describe_error(int code, char *buffer, size_t size)
{
#ifdef _WIN32
    /* English avoids ANSI-codepage text in UTF-8 PHP; fall back to the code. */
    DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD) code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        buffer, (DWORD) size, NULL);
    while (length > 0 && (buffer[length - 1] == '\r' || buffer[length - 1] == '\n'
        || buffer[length - 1] == ' ' || buffer[length - 1] == '.')) {
        buffer[--length] = '\0';
    }
    if (length == 0) {
        snprintf(buffer, size, "Windows socket error %d", code);
    }
    return buffer;
#else
    snprintf(buffer, size, "%s", strerror(code));
    return buffer;
#endif
}

static bool wait_socket(riak_socket fd, riak_wait direction, int64_t deadline,
    char *error, size_t error_size)
{
    char reason[256];

    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) {
            snprintf(error, error_size, "Riak PB operation timed out");
            return false;
        }
        int64_t remaining = deadline - now;
#ifdef _WIN32
        /*
         * select() rather than WSAPoll(): older WSAPoll builds never report a
         * failed non-blocking connect. Failed connects land in the except set.
         */
        fd_set read_set, write_set, except_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        if (direction == RIAK_WAIT_READ) {
            FD_SET(fd, &read_set);
        } else {
            FD_SET(fd, &write_set);
            FD_SET(fd, &except_set);
        }
        struct timeval timeout;
        timeout.tv_sec = (long) (remaining / 1000);
        timeout.tv_usec = (long) (remaining % 1000) * 1000;
        int result = select(0, &read_set, &write_set, &except_set, &timeout);
        if (result > 0) {
            return true;
        }
#else
        struct pollfd descriptor = {fd, (short) (direction == RIAK_WAIT_READ ? POLLIN : POLLOUT), 0};
        int result = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int) remaining);
        if (result > 0) {
            if (descriptor.revents & POLLNVAL) {
                snprintf(error, error_size, "Riak PB socket is invalid");
                return false;
            }
            /* Let send/recv/getsockopt report EOF, reset, and connect errors. */
            return true;
        }
#endif
        if (result == 0) {
            snprintf(error, error_size, "Riak PB operation timed out");
            return false;
        }
        int code = last_socket_error();
        if (code != RIAK_ERR_INTR) {
            snprintf(error, error_size, "Riak PB poll failed: %s",
                describe_error(code, reason, sizeof(reason)));
            return false;
        }
    }
}

static riak_socket open_socket(const struct addrinfo *address)
{
#ifdef _WIN32
    /* Overlapped matches socket(); NO_HANDLE_INHERIT is the FD_CLOEXEC analogue. */
    riak_socket fd = WSASocketW(address->ai_family, address->ai_socktype,
        address->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (fd == INVALID_SOCKET) {
        return fd;
    }
    u_long non_blocking = 1;
    if (ioctlsocket(fd, FIONBIO, &non_blocking) != 0) {
        int code = WSAGetLastError();
        closesocket(fd);
        WSASetLastError(code);
        return INVALID_SOCKET;
    }
    return fd;
#else
    riak_socket fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
        return fd;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
        || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int code = errno;
        close(fd);
        errno = code;
        return RIAK_INVALID_SOCKET;
    }
    return fd;
#endif
}

static riak_socket connect_socket(const char *host, unsigned int port, int64_t deadline,
    char *error, size_t error_size)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    char service[6];
    char reason[256];
    riak_socket connected_fd = RIAK_INVALID_SOCKET;
    int last_error = RIAK_ERR_CONNREFUSED;

    memset(&hints, 0, sizeof(hints));
    snprintf(service, sizeof(service), "%u", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int result = getaddrinfo(host, service, &hints, &addresses);
    if (result != 0) {
#ifdef _WIN32
        /* getaddrinfo returns a WSA code; gai_strerror is not thread-safe here. */
        snprintf(error, error_size, "Riak PB address lookup failed: %s",
            describe_error(result, reason, sizeof(reason)));
#else
        snprintf(error, error_size, "Riak PB address lookup failed: %s", gai_strerror(result));
#endif
        return RIAK_INVALID_SOCKET;
    }

    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) {
            last_error = RIAK_ERR_TIMEDOUT;
            break;
        }
        riak_socket fd = open_socket(address);
        if (fd == RIAK_INVALID_SOCKET) {
            last_error = last_socket_error();
            continue;
        }
#ifdef _WIN32
        result = connect(fd, address->ai_addr, (int) address->ai_addrlen);
#else
        result = connect(fd, address->ai_addr, address->ai_addrlen);
#endif
        int connect_error = result != 0 ? last_socket_error() : 0;
#ifdef _WIN32
        bool in_progress = connect_error == WSAEWOULDBLOCK;
#else
        bool in_progress = connect_error == EINPROGRESS;
#endif
        if (result != 0 && in_progress) {
            if (!wait_socket(fd, RIAK_WAIT_WRITE, deadline, error, error_size)) {
                riak_close_socket(fd);
                freeaddrinfo(addresses);
                return RIAK_INVALID_SOCKET;
            }
#ifdef _WIN32
            int size = (int) sizeof(last_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &last_error, &size) != 0) {
#else
            socklen_t size = sizeof(last_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &last_error, &size) < 0) {
#endif
                last_error = last_socket_error();
            }
            result = last_error == 0 ? 0 : -1;
        } else if (result != 0) {
            last_error = connect_error;
        }
        if (result == 0) {
            connected_fd = fd;
            break;
        }
        riak_close_socket(fd);
    }
    freeaddrinfo(addresses);
    if (connected_fd == RIAK_INVALID_SOCKET) {
        snprintf(error, error_size, "Riak PB connect to %s:%u failed: %s",
            host, port, describe_error(last_error, reason, sizeof(reason)));
    }
    return connected_fd;
}

static bool transfer_bytes(riak_socket fd, void *buffer, size_t length, bool writing,
    int64_t deadline, char *error, size_t error_size)
{
    char reason[256];
    size_t offset = 0;

    while (offset < length) {
        if (!wait_socket(fd, writing ? RIAK_WAIT_WRITE : RIAK_WAIT_READ,
                deadline, error, error_size)) {
            return false;
        }
#ifdef _WIN32
        /* Winsock lengths are int. */
        size_t chunk = length - offset;
        int request = chunk > INT_MAX ? INT_MAX : (int) chunk;
        int count = writing
            ? send(fd, (const char *) buffer + offset, request, 0)
            : recv(fd, (char *) buffer + offset, request, 0);
#else
        ssize_t count = writing
            ? send(fd, (const char *) buffer + offset, length - offset, MSG_NOSIGNAL)
            : recv(fd, (char *) buffer + offset, length - offset, 0);
#endif
        if (count > 0) {
            offset += (size_t) count;
            continue;
        }
        if (count == 0) {
            snprintf(error, error_size, "Riak PB connection closed before the frame was complete");
            return false;
        }
        int code = last_socket_error();
        if (code != RIAK_ERR_INTR && !is_would_block(code)) {
            snprintf(error, error_size, "Riak PB %s failed: %s",
                writing ? "write" : "read", describe_error(code, reason, sizeof(reason)));
            return false;
        }
    }
    return true;
}

void riak_reply_free(riak_reply *reply)
{
    free(reply->data);
    reply->data = NULL;
    reply->length = 0;
    reply->code = 0;
}

bool riak_exchange(const char *host, unsigned int port, unsigned int timeout_ms,
    uint8_t code, const uint8_t *payload, size_t length, riak_reply *reply,
    char *error, size_t error_size)
{
    memset(reply, 0, sizeof(*reply));
    if (!host || !*host || port == 0 || port > 65535 || timeout_ms == 0
        || timeout_ms > 600000 || (length > 0 && !payload)) {
        snprintf(error, error_size, "Invalid Riak PB transport parameters");
        return false;
    }
    if (length >= RIAK_MAX_FRAME_SIZE) {
        snprintf(error, error_size, "Riak PB request exceeds the 64 MiB frame limit");
        return false;
    }
    int64_t start = monotonic_ms();
    if (start < 0) {
        snprintf(error, error_size, "Riak PB monotonic clock is unavailable");
        return false;
    }
    int64_t deadline = start + timeout_ms;
    riak_socket fd = connect_socket(host, port, deadline, error, error_size);
    if (fd == RIAK_INVALID_SOCKET) {
        return false;
    }
    uint32_t size = (uint32_t) length + 1;
    uint8_t header[5] = {
        (uint8_t) (size >> 24), (uint8_t) (size >> 16),
        (uint8_t) (size >> 8), (uint8_t) size, code
    };
    bool success = false;
    if (!transfer_bytes(fd, header, sizeof(header), true, deadline, error, error_size)
        || !transfer_bytes(fd, (void *) payload, length, true, deadline, error, error_size)
        || !transfer_bytes(fd, header, 4, false, deadline, error, error_size)) {
        goto cleanup;
    }
    size = ((uint32_t) header[0] << 24) | ((uint32_t) header[1] << 16)
        | ((uint32_t) header[2] << 8) | header[3];
    if (size == 0 || size > RIAK_MAX_FRAME_SIZE) {
        snprintf(error, error_size, "Riak PB response has an invalid frame length (limit 64 MiB)");
        goto cleanup;
    }
    if (!transfer_bytes(fd, &reply->code, 1, false, deadline, error, error_size)) {
        goto cleanup;
    }
    reply->length = size - 1;
    if (reply->length > 0) {
        reply->data = malloc(reply->length);
        if (!reply->data) {
            snprintf(error, error_size, "Cannot allocate Riak PB response buffer");
            goto cleanup;
        }
        if (!transfer_bytes(fd, reply->data, reply->length, false, deadline, error, error_size)) {
            goto cleanup;
        }
    }
    success = true;
cleanup:
    riak_close_socket(fd);
    if (!success) {
        riak_reply_free(reply);
    }
    return success;
}
