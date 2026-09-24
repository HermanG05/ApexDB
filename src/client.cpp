#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "list.h"
#include "protocol.hpp"
#include "cli.hpp"
#include "tcp_rpc.hpp"

static int32_t print_response(const uint8_t *data, size_t size, size_t depth = 0) {
    if (size < 1 || depth > 64) {
        return -1;
    }
    switch (data[0]) {
    case TAG_NIL:
        printf("(nil)\n");
        return 1;
    case TAG_ERR:
        if (size < 1 + 8) {
            return -1;
        }
        {
            int32_t code = 0;
            uint32_t len = 0;
            memcpy(&code, &data[1], 4);
            memcpy(&len, &data[1 + 4], 4);
            if (len > size - 9) {
                return -1;
            }
            printf("(err) %d %.*s\n", code, static_cast<int>(len), &data[1 + 8]);
            return static_cast<int32_t>(1 + 8 + len);
        }
    case TAG_STR:
        if (size < 1 + 4) {
            return -1;
        }
        {
            uint32_t len = 0;
            memcpy(&len, &data[1], 4);
            if (len > size - 5) {
                return -1;
            }
            printf("(str) %.*s\n", static_cast<int>(len), &data[1 + 4]);
            return static_cast<int32_t>(1 + 4 + len);
        }
    case TAG_INT:
        if (size < 1 + 8) {
            return -1;
        }
        {
            int64_t val = 0;
            memcpy(&val, &data[1], 8);
            printf("(int) %lld\n", static_cast<long long>(val));
            return static_cast<int32_t>(1 + 8);
        }
    case TAG_DBL:
        if (size < 1 + 8) {
            return -1;
        }
        {
            double val = 0;
            memcpy(&val, &data[1], 8);
            printf("(dbl) %g\n", val);
            return static_cast<int32_t>(1 + 8);
        }
    case TAG_ARR:
        if (size < 1 + 4) {
            return -1;
        }
        {
            uint32_t len = 0;
            memcpy(&len, &data[1], 4);
            printf("(arr) len=%u\n", len);
            size_t arr_bytes = 1 + 4;
            for (uint32_t i = 0; i < len; ++i) {
                int32_t rv = print_response(data + arr_bytes, size - arr_bytes, depth + 1);
                if (rv < 0) {
                    return rv;
                }
                arr_bytes += static_cast<size_t>(rv);
            }
            printf("(arr) end\n");
            return static_cast<int32_t>(arr_bytes);
        }
    default:
        return -1;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-h HOST] [-p PORT] <command> [args...]\n", prog);
}

int main(int argc, char **argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::vector<std::string> cmd;

    bool command_started = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (command_started) {
            cmd.push_back(arg);
        } else if (arg == "-h" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            if (!parse_port(argv[++i], port)) {
                usage(argv[0]);
                return 1;
            }
        } else if (arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            command_started = true;
            cmd.push_back(arg);
        }
    }

    if (cmd.empty()) {
        usage(argv[0]);
        return 1;
    }

    Buffer response;
    if (!tcp_rpc(host, port, cmd, response) || response.size() <= 4) {
        fprintf(stderr, "request failed\n");
        return 1;
    }
    int32_t consumed = print_response(response.data() + 4, response.size() - 4);
    if (consumed < 0 || static_cast<size_t>(consumed) != response.size() - 4) {
        fprintf(stderr, "invalid response\n");
        return 1;
    }
    return response[4] == TAG_ERR ? 1 : 0;
}
