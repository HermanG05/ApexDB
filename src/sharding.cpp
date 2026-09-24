#include "sharding.hpp"

#include "common.h"

size_t shard_for_key(const std::string &key, size_t num_shards) {
    if (num_shards == 0) {
        return 0;
    }
    uint64_t h = str_hash(reinterpret_cast<const uint8_t *>(key.data()), key.size());
    return static_cast<size_t>(h % num_shards);
}

std::optional<std::string> routing_key_for_command(const std::vector<std::string> &cmd) {
    if (cmd.empty()) {
        return std::nullopt;
    }
    const std::string &op = cmd[0];
    if (op == "get" || op == "set" || op == "del" || op == "pexpire" || op == "pttl" || op == "exists" || op == "type" || op == "persist" || op == "incr") {
        if (cmd.size() >= 2) {
            return cmd[1];
        }
    }
    if (op == "zadd" || op == "zrem" || op == "zscore" || op == "zquery") {
        if (cmd.size() >= 2) {
            return cmd[1];
        }
    }
    return std::nullopt;
}

bool is_scatter_gather_command(const std::vector<std::string> &cmd) {
    return cmd.size() == 1 && cmd[0] == "keys";
}
