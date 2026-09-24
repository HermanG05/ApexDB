#include "protocol.hpp"

#include <cassert>
#include <cstring>

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (static_cast<size_t>(end - cur) < 4) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    if (n > static_cast<size_t>(end - cur)) {
        return false;
    }
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

bool parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    out.clear();
    if (size < 4 || size > k_max_msg) {
        return false;
    }
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) {
        return false;
    }
    if (nstr > k_max_args) {
        return false;
    }

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) {
            return false;
        }
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) {
            return false;
        }
    }
    return data == end;
}

void buf_append(Buffer &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

void buf_consume(Buffer &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n));
}

static void buf_append_u8(Buffer &buf, uint8_t data) {
    buf.push_back(data);
}

static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, reinterpret_cast<const uint8_t *>(&data), 4);
}

static void buf_append_i64(Buffer &buf, int64_t data) {
    buf_append(buf, reinterpret_cast<const uint8_t *>(&data), 8);
}

static void buf_append_dbl(Buffer &buf, double data) {
    buf_append(buf, reinterpret_cast<const uint8_t *>(&data), 8);
}

void out_nil(Buffer &out) {
    buf_append_u8(out, TAG_NIL);
}

void out_str(Buffer &out, const char *s, size_t size) {
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, static_cast<uint32_t>(size));
    buf_append(out, reinterpret_cast<const uint8_t *>(s), size);
}

void out_str(Buffer &out, const std::string &s) {
    out_str(out, s.data(), s.size());
}

void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}

void out_dbl(Buffer &out, double val) {
    buf_append_u8(out, TAG_DBL);
    buf_append_dbl(out, val);
}

void out_err(Buffer &out, uint32_t code, const std::string &msg) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, static_cast<uint32_t>(msg.size()));
    buf_append(out, reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

void out_arr(Buffer &out, uint32_t n) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, n);
}

size_t out_begin_arr(Buffer &out) {
    out.push_back(TAG_ARR);
    buf_append_u32(out, 0);
    return out.size() - 4;
}

void out_end_arr(Buffer &out, size_t ctx, uint32_t n) {
    assert(out[ctx - 1] == TAG_ARR);
    memcpy(&out[ctx], &n, 4);
}

void response_begin(Buffer &out, size_t &header) {
    header = out.size();
    buf_append_u32(out, 0);
}

static size_t response_size(const Buffer &out, size_t header) {
    return out.size() - header - 4;
}

void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) {
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG, "response is too big.");
        msg_size = response_size(out, header);
    }
    uint32_t len = static_cast<uint32_t>(msg_size);
    memcpy(&out[header], &len, 4);
}

Buffer encode_request(const std::vector<std::string> &cmd) {
    if (cmd.size() > k_max_args) {
        return {};
    }
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        if (len > k_max_msg - 4 || s.size() > k_max_msg - len - 4) {
            return {};
        }
        len += 4 + static_cast<uint32_t>(s.size());
    }
    Buffer buf;
    buf.reserve(4 + len);
    buf_append_u32(buf, len);
    buf_append_u32(buf, static_cast<uint32_t>(cmd.size()));
    for (const std::string &s : cmd) {
        buf_append_u32(buf, static_cast<uint32_t>(s.size()));
        buf_append(buf, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }
    return buf;
}
