#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "kv_store.hpp"
#include "list.h"
#include "protocol.hpp"
#include "cli.hpp"
#include "server_loop.hpp"
#include "sharding.hpp"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--id SHARD_ID] [--shards NUM_SHARDS] [--port PORT]\n"
            "  Defaults: id=0, shards=1, port=7000\n",
            prog);
}

int main(int argc, char **argv) {
    size_t shard_id = 0;
    size_t num_shards = 1;
    uint16_t port = 7000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            uint64_t value = 0;
            if (!parse_unsigned(argv[++i], value, SIZE_MAX)) {
                usage(argv[0]);
                return 1;
            }
            shard_id = static_cast<size_t>(value);
        } else if (arg == "--shards" && i + 1 < argc) {
            uint64_t value = 0;
            if (!parse_unsigned(argv[++i], value, SIZE_MAX)) {
                usage(argv[0]);
                return 1;
            }
            num_shards = static_cast<size_t>(value);
        } else if (arg == "--port" && i + 1 < argc) {
            if (!parse_port(argv[++i], port)) {
                usage(argv[0]);
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (shard_id >= num_shards) {
        fprintf(stderr, "shard id must be in [0, num_shards)\n");
        return 1;
    }

    KVStore store;
    store.init(4);

    fprintf(stderr, "shard %zu / %zu on port %u\n", shard_id, num_shards, port);

    auto on_request = [&](const std::vector<std::string> &cmd, Buffer &out) {
        if (num_shards > 1) {
            auto key = routing_key_for_command(cmd);
            if (key && shard_for_key(*key, num_shards) != shard_id) {
                return out_err(out, ERR_BAD_ARG, "key does not belong to this shard");
            }
        }
        store.execute(cmd, out);
    };

    auto on_timer = [&](uint64_t now_ms) { store.process_timers(now_ms); };
    auto next_timer = [&](uint64_t now_ms) { return store.next_timer_ms(now_ms); };

    run_tcp_server(port, on_request, on_timer, next_timer);
    return 0;
}
