#include "protocol.hpp"

#include <cassert>
#include <cstring>

int main() {
    std::vector<std::string> command{"set", "key", std::string("x\0y", 3)};
    Buffer request = encode_request(command);
    std::vector<std::string> parsed{"old"};
    assert(parse_req(request.data() + 4, request.size() - 4, parsed));
    assert(parsed == command);
    for (size_t size = 0; size < request.size() - 4; ++size) {
        assert(!parse_req(request.data() + 4, size, parsed));
    }
    Buffer malformed{1, 0, 0, 0, 255, 255, 255, 255};
    assert(!parse_req(malformed.data(), malformed.size(), parsed));
    request.push_back(0);
    assert(!parse_req(request.data() + 4, request.size() - 4, parsed));
    assert(encode_request({std::string(k_max_msg, 'x')}).empty());
    assert(encode_request(std::vector<std::string>(k_max_args + 1)).empty());
    Buffer out;
    size_t header;
    response_begin(out, header);
    out_str(out, std::string(k_max_msg, 'x'));
    response_end(out, header);
    assert(out[4] == TAG_ERR);
    uint32_t length;
    memcpy(&length, out.data(), 4);
    assert(length == out.size() - 4);
}
