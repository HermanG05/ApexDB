#include "server_loop.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <algorithm>
#include <climits>
#include <vector>

#include "common.h"
#include "list.h"

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static uint64_t get_monotonic_msec() {
    struct timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_nsec / 1000 / 1000;
}

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        die("fcntl");
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    Buffer incoming;
    Buffer outgoing;
    uint64_t last_active_ms = 0;
    DList idle_node;
};

static volatile std::sig_atomic_t stopping = 0;

static void stop_server(int) {
    stopping = 1;
}

static struct {
    std::vector<Conn *> fd2conn;
    DList idle_list;
    RequestHandler on_request;
    TimerHandler on_timer;
    TimerDeadlineFn next_timer_ms;
} g_srv;

constexpr uint64_t k_idle_timeout_ms = 5 * 1000;

static int32_t handle_accept(int fd) {
    sockaddr_in client_addr{};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, reinterpret_cast<sockaddr *>(&client_addr), &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return -1;
    }
    fd_set_nb(connfd);

    auto *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    conn->last_active_ms = get_monotonic_msec();
    dlist_insert_before(&g_srv.idle_list, &conn->idle_node);

    if (g_srv.fd2conn.size() <= static_cast<size_t>(conn->fd)) {
        g_srv.fd2conn.resize(conn->fd + 1);
    }
    assert(!g_srv.fd2conn[conn->fd]);
    g_srv.fd2conn[conn->fd] = conn;
    return 0;
}

static void conn_destroy(Conn *conn) {
    close(conn->fd);
    g_srv.fd2conn[conn->fd] = nullptr;
    dlist_detach(&conn->idle_node);
    delete conn;
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        conn->want_close = true;
        return false;
    }
    if (4 + len > conn->incoming.size()) {
        return false;
    }
    const uint8_t *request = conn->incoming.data() + 4;

    std::vector<std::string> cmd;
    if (!parse_req(request, len, cmd)) {
        conn->want_close = true;
        return false;
    }

    size_t header_pos = 0;
    response_begin(conn->outgoing, header_pos);
    g_srv.on_request(cmd, conn->outgoing);
    response_end(conn->outgoing, header_pos);

    buf_consume(conn->incoming, 4 + len);
    return true;
}

static void handle_write(Conn *conn) {
    assert(!conn->outgoing.empty());
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }
    buf_consume(conn->outgoing, static_cast<size_t>(rv));
    if (conn->outgoing.empty()) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }
    if (rv == 0) {
        conn->want_close = true;
        return;
    }
    buf_append(conn->incoming, buf, static_cast<size_t>(rv));
    while (try_one_request(conn)) {
    }
    if (!conn->outgoing.empty()) {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn);
    }
}

static uint32_t next_timer_ms_combined() {
    uint64_t now_ms = get_monotonic_msec();
    uint64_t next_ms = static_cast<uint64_t>(-1);

    if (!dlist_empty(&g_srv.idle_list)) {
        Conn *conn = container_of(g_srv.idle_list.next, Conn, idle_node);
        next_ms = conn->last_active_ms + k_idle_timeout_ms;
    }

    if (g_srv.next_timer_ms) {
        uint32_t store_ms = g_srv.next_timer_ms(now_ms);
        if (store_ms != static_cast<uint32_t>(-1)) {
            uint64_t store_deadline = now_ms + store_ms;
            if (store_deadline < next_ms) {
                next_ms = store_deadline;
            }
        }
    }

    if (next_ms == static_cast<uint64_t>(-1)) {
        return static_cast<uint32_t>(-1);
    }
    if (next_ms <= now_ms) {
        return 0;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(next_ms - now_ms, INT_MAX));
}

static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();
    while (!dlist_empty(&g_srv.idle_list)) {
        Conn *conn = container_of(g_srv.idle_list.next, Conn, idle_node);
        if (conn->last_active_ms + k_idle_timeout_ms >= now_ms) {
            break;
        }
        conn_destroy(conn);
    }
    if (g_srv.on_timer) {
        g_srv.on_timer(now_ms);
    }
}

void run_tcp_server(uint16_t port, RequestHandler on_request,
                    TimerHandler on_timer, TimerDeadlineFn next_timer_ms) {
    stopping = 0;
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);
    g_srv.on_request = std::move(on_request);
    g_srv.on_timer = std::move(on_timer);
    g_srv.next_timer_ms = std::move(next_timer_ms);
    dlist_init(&g_srv.idle_list);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
        die("bind()");
    }

    fd_set_nb(fd);
    if (listen(fd, SOMAXCONN)) {
        die("listen()");
    }

    fprintf(stderr, "listening on 0.0.0.0:%u\n", port);

    std::vector<pollfd> poll_args;
    while (!stopping) {
        poll_args.clear();
        poll_args.push_back({fd, POLLIN, 0});
        for (Conn *conn : g_srv.fd2conn) {
            if (!conn) {
                continue;
            }
            pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read) {
                pfd.events |= POLLIN;
            }
            if (conn->want_write) {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), static_cast<nfds_t>(poll_args.size()),
                      static_cast<int>(std::min<uint32_t>(next_timer_ms_combined(), 1000)));
        if (rv < 0 && errno == EINTR) {
            continue;
        }
        if (rv < 0) {
            die("poll");
        }

        if (poll_args[0].revents) {
            handle_accept(fd);
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) {
                continue;
            }
            Conn *conn = g_srv.fd2conn[poll_args[i].fd];
            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_srv.idle_list, &conn->idle_node);

            if (ready & POLLIN) {
                handle_read(conn);
            }
            if ((ready & POLLOUT) && !conn->want_close && !conn->outgoing.empty()) {
                handle_write(conn);
            }
            if ((ready & (POLLERR | POLLHUP | POLLNVAL)) || conn->want_close) {
                conn_destroy(conn);
            }
        }
        process_timers();
    }
    for (Conn *conn : g_srv.fd2conn) {
        if (conn) {
            conn_destroy(conn);
        }
    }
    g_srv.fd2conn.clear();
    close(fd);
}
