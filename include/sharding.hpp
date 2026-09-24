#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ShardEndpoint {
    std::string host;
    uint16_t port = 0;
};

// FNV-1a hash partitioned sharding (same hash as the storage engine).
size_t shard_for_key(const std::string &key, size_t num_shards);

std::optional<std::string> routing_key_for_command(const std::vector<std::string> &cmd);

bool is_scatter_gather_command(const std::vector<std::string> &cmd);
