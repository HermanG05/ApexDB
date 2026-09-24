#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.hpp"

bool tcp_rpc(const std::string &host, uint16_t port,
             const std::vector<std::string> &cmd, Buffer &response_out);

bool tcp_rpc_raw(const std::string &host, uint16_t port,
                 const Buffer &request, Buffer &response_out);
