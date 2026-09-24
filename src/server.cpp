// Single-node server (shard 0 of 1). Use proxy + shard for distributed mode.
#include <cstdio>
#include "cli.hpp"

#include "kv_store.hpp"
#include "server_loop.hpp"

int main(int argc, char **argv) {
    uint16_t port = 6379;
    if (argc == 2 && std::string(argv[1]) == "--help") {
        fprintf(stderr, "Usage: %s [--port PORT]\n", argv[0]);
        return 0;
    }
    if (argc != 1 && (argc != 3 || std::string(argv[1]) != "--port" || !parse_port(argv[2], port))) {
        fprintf(stderr, "Usage: %s [--port PORT]\n", argv[0]);
        return 1;
    }
    KVStore store;
    store.init(4);

    fprintf(stderr, "single-node server on port %u\n", port);

    auto on_request = [&](const std::vector<std::string> &cmd, Buffer &out) {
        store.execute(cmd, out);
    };
    auto on_timer = [&](uint64_t now_ms) { store.process_timers(now_ms); };
    auto next_timer = [&](uint64_t now_ms) { return store.next_timer_ms(now_ms); };

    run_tcp_server(port, on_request, on_timer, next_timer);
    return 0;
}
