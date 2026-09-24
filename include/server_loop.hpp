#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>

#include "protocol.hpp"

using RequestHandler = std::function<void(const std::vector<std::string> &, Buffer &)>;
using TimerHandler = std::function<void(uint64_t now_ms)>;
using TimerDeadlineFn = std::function<uint32_t(uint64_t now_ms)>;

// Non-blocking TCP server with length-prefixed request/response framing.
void run_tcp_server(uint16_t port, RequestHandler on_request,
                    TimerHandler on_timer, TimerDeadlineFn next_timer_ms);
