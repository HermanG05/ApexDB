#include "tcp_rpc.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <cerrno>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <vector>

static bool read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv < 0 && errno == EINTR) {
            continue;
        }
        if (rv <= 0) {
            return false;
        }
        n -= static_cast<size_t>(rv);
        buf += rv;
    }
    return true;
}

static bool write_full(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        #ifdef MSG_NOSIGNAL
        ssize_t rv = send(fd, buf, n, MSG_NOSIGNAL);
#else
        ssize_t rv = send(fd, buf, n, 0);
#endif
        if (rv < 0 && errno == EINTR) {
            continue;
        }
        if (rv <= 0) {
            return false;
        }
        n -= static_cast<size_t>(rv);
        buf += rv;
    }
    return true;
}

bool tcp_rpc_raw(const std::string &host, uint16_t port,
                 const Buffer &request, Buffer &response_out) {
    response_out.clear();
    if (request.size() < 4 || request.size() - 4 > k_max_msg) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses)) {
        return false;
    }
    int fd = -1;
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int connected = connect(fd, address->ai_addr, address->ai_addrlen);
        if (connected < 0 && errno == EINPROGRESS) {
            pollfd pending{fd, POLLOUT, 0};
            int error = 0;
            socklen_t length = sizeof(error);
            if (poll(&pending, 1, 2000) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error) {
                connected = 0;
            }
        }
        if (connected == 0 && fcntl(fd, F_SETFL, flags) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        return false;
    }

    if (!write_full(fd, request.data(), request.size())) {
        close(fd);
        return false;
    }

    uint8_t hdr[4];
    if (!read_full(fd, hdr, 4)) {
        close(fd);
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, hdr, 4);
    if (len > k_max_msg) {
        close(fd);
        return false;
    }

    response_out.resize(4 + len);
    memcpy(response_out.data(), hdr, 4);
    if (!read_full(fd, response_out.data() + 4, len)) {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool tcp_rpc(const std::string &host, uint16_t port,
             const std::vector<std::string> &cmd, Buffer &response_out) {
    Buffer req = encode_request(cmd);
    return tcp_rpc_raw(host, port, req, response_out);
}
