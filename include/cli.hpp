#pragma once

#include <charconv>
#include <cstdint>
#include <string>

inline bool parse_unsigned(const std::string &text, uint64_t &value, uint64_t maximum) {
    if (text.empty()) {
        return false;
    }
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= maximum;
}

inline bool parse_port(const std::string &text, uint16_t &port) {
    uint64_t value = 0;
    if (!parse_unsigned(text, value, 65535) || value == 0) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}
