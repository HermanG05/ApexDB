#include "kv_store.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

static Buffer run(KVStore &store, std::vector<std::string> cmd) {
    Buffer out;
    store.execute(cmd, out);
    assert(!out.empty());
    return out;
}

static void integer(KVStore &store, std::vector<std::string> cmd, int64_t expected) {
    Buffer out = run(store, cmd);
    assert(out.size() == 9 && out[0] == TAG_INT);
    int64_t actual;
    memcpy(&actual, out.data() + 1, 8);
    assert(actual == expected);
}

static void string(KVStore &store, std::vector<std::string> cmd, std::string expected) {
    Buffer out = run(store, cmd);
    Buffer wanted;
    out_str(wanted, expected);
    assert(out == wanted);
}

int main() {
    KVStore store;
    store.init(2);
    string(store, {"ping"}, "PONG");
    string(store, {"ping", "hello"}, "hello");
    assert(run(store, {"get", "missing"})[0] == TAG_NIL);
    string(store, {"set", "k", std::string("a\0b", 3)}, "OK");
    string(store, {"get", "k"}, std::string("a\0b", 3));
    integer(store, {"exists", "k"}, 1);
    string(store, {"type", "k"}, "string");
    integer(store, {"pttl", "k"}, -1);
    integer(store, {"pexpire", "k", "10000"}, 1);
    integer(store, {"persist", "k"}, 1);
    integer(store, {"persist", "k"}, 0);
    integer(store, {"pexpire", "k", "1"}, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    integer(store, {"exists", "k"}, 0);
    integer(store, {"pttl", "k"}, -2);
    string(store, {"set", "k", "1"}, "OK");
    integer(store, {"pexpire", "k", "10000"}, 1);
    integer(store, {"incr", "k"}, 2);
    integer(store, {"persist", "k"}, 1);
    integer(store, {"pexpire", "k", "10000"}, 1);
    string(store, {"set", "k", "3"}, "OK");
    integer(store, {"pttl", "k"}, -1);
    integer(store, {"pexpire", "k", "0"}, 1);
    integer(store, {"exists", "k"}, 0);
    integer(store, {"incr", "k"}, 1);
    for (const char *value : {"", " 1", "1x", "9223372036854775808", "9223372036854775807"}) {
        string(store, {"set", "k", value}, "OK");
        assert(run(store, {"incr", "k"})[0] == TAG_ERR);
        string(store, {"get", "k"}, value);
    }
    for (const char *value : {"", "1x", "9223372036854775808"}) {
        assert(run(store, {"pexpire", "k", value})[0] == TAG_ERR);
    }
    integer(store, {"del", "k"}, 1);
    integer(store, {"del", "k"}, 0);
    integer(store, {"zadd", "z", "1", "a"}, 1);
    integer(store, {"zadd", "z", "2", "b"}, 1);
    integer(store, {"zadd", "z", "3", "a"}, 0);
    string(store, {"type", "z"}, "zset");
    assert(run(store, {"get", "z"})[0] == TAG_ERR);
    assert(run(store, {"incr", "z"})[0] == TAG_ERR);
    assert(run(store, {"zadd", "z", "nan", "x"})[0] == TAG_ERR);
    assert(run(store, {"zadd", "z", "", "x"})[0] == TAG_ERR);
    Buffer expected;
    out_arr(expected, 4);
    out_str(expected, "b");
    out_dbl(expected, 2);
    out_str(expected, "a");
    out_dbl(expected, 3);
    assert(run(store, {"zquery", "z", "0", "", "0", "4"}) == expected);
    integer(store, {"zrem", "z", "a"}, 1);
    integer(store, {"del", "z"}, 1);
    string(store, {"type", "z"}, "none");
    assert(run(store, {})[0] == TAG_ERR);
    assert(run(store, {"keys", "extra"})[0] == TAG_ERR);
    for (int i = 0; i < 5000; ++i) {
        string(store, {"set", std::to_string(i), "v"}, "OK");
    }
    for (int i = 0; i < 5000; ++i) {
        string(store, {"get", std::to_string(i)}, "v");
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
        KVStore temporary;
        if (cycle != 0) {
            temporary.init(2);
        }
        for (int i = 0; i < 1200; ++i) {
            integer(temporary, {"zadd", "large", std::to_string(i), std::to_string(i)}, 1);
        }
        integer(temporary, {"del", "large"}, 1);
    }
}
