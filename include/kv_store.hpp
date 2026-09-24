#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.hpp"

class KVStore {
public:
    KVStore();
    ~KVStore();

    void init(size_t thread_pool_threads = 4);
    void execute(const std::vector<std::string> &cmd, Buffer &out);
    void process_timers(uint64_t now_ms);
    uint32_t next_timer_ms(uint64_t now_ms) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
