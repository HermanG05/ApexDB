#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "list.h"

using Buffer = std::vector<uint8_t>;

constexpr size_t k_max_msg = 32 << 20;
constexpr size_t k_max_args = 200 * 1000;

bool parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out);

void buf_append(Buffer &buf, const uint8_t *data, size_t len);
void buf_consume(Buffer &buf, size_t n);

void out_nil(Buffer &out);
void out_str(Buffer &out, const char *s, size_t size);
void out_str(Buffer &out, const std::string &s);
void out_int(Buffer &out, int64_t val);
void out_dbl(Buffer &out, double val);
void out_err(Buffer &out, uint32_t code, const std::string &msg);
void out_arr(Buffer &out, uint32_t n);
size_t out_begin_arr(Buffer &out);
void out_end_arr(Buffer &out, size_t ctx, uint32_t n);

void response_begin(Buffer &out, size_t &header);
void response_end(Buffer &out, size_t header);

Buffer encode_request(const std::vector<std::string> &cmd);
