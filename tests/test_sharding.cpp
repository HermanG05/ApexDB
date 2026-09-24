#include <cassert>
#include <cstdio>
#include <string>

#include "sharding.hpp"
#include "common.h"

int main() {
    assert(shard_for_key("a", 3) < 3);
    assert(shard_for_key("a", 3) == shard_for_key("a", 3));
    assert(str_hash(reinterpret_cast<const uint8_t *>("hello"), 5) == 0x4f9f2cab);
    assert(shard_for_key("a", 3) == 1);
    assert(shard_for_key("b", 3) == 1);
    for (const char *op : {"exists", "type", "persist", "incr"}) {
        assert(routing_key_for_command({op, "k"}) == "k");
    }
    assert(!routing_key_for_command({"unknown", "k"}));
    assert(!is_scatter_gather_command({"keys", "extra"}));

    std::vector<std::string> get_cmd = {"get", "mykey"};
    auto key = routing_key_for_command(get_cmd);
    assert(key && *key == "mykey");

    std::vector<std::string> keys_cmd = {"keys"};
    assert(is_scatter_gather_command(keys_cmd));

    printf("test_sharding: ok\n");
    return 0;
}
