#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.hpp"
#include "cli.hpp"
#include "server_loop.hpp"
#include "sharding.hpp"
#include "tcp_rpc.hpp"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--port PORT] --shard HOST:PORT [--shard HOST:PORT ...]\n"
            "  Example: %s --port 6379 --shard 127.0.0.1:7000 --shard 127.0.0.1:7001\n",
            prog, prog);
}

static bool parse_host_port(const std::string &s, ShardEndpoint &ep) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    ep.host = s.substr(0, colon);
    return parse_port(s.substr(colon + 1), ep.port);
}

// Merge TAG_ARR payloads from shard responses (body only, after 4-byte length).
static bool merge_key_arrays(const std::vector<Buffer> &bodies, Buffer &out) {
    std::vector<std::string> keys;
    for (const Buffer &body : bodies) {
        if (body.empty() || body[0] != TAG_ARR || body.size() < 5) {
            return false;
        }
        uint32_t n = 0;
        memcpy(&n, &body[1], 4);
        size_t off = 5;
        for (uint32_t i = 0; i < n; ++i) {
            if (body.size() - off < 5 || body[off] != TAG_STR) {
                return false;
            }
            uint32_t slen = 0;
            memcpy(&slen, &body[off + 1], 4);
            if (off + 5 + slen > body.size()) {
                return false;
            }
            keys.emplace_back(reinterpret_cast<const char *>(&body[off + 5]), slen);
            off += 5 + slen;
        }
        if (off != body.size()) {
            return false;
        }
    }
    out_arr(out, static_cast<uint32_t>(keys.size()));
    for (const std::string &k : keys) {
        out_str(out, k);
    }
    return true;
}

static bool extract_response_body(const Buffer &framed, Buffer &body) {
    if (framed.size() < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, framed.data(), 4);
    if (len != framed.size() - 4) {
        return false;
    }
    body.assign(framed.begin() + 4, framed.begin() + 4 + len);
    return true;
}

int main(int argc, char **argv) {
    uint16_t port = 6379;
    std::vector<ShardEndpoint> shards;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            if (!parse_port(argv[++i], port)) {
                usage(argv[0]);
                return 1;
            }
        } else if (arg == "--shard" && i + 1 < argc) {
            ShardEndpoint ep;
            if (!parse_host_port(argv[++i], ep)) {
                usage(argv[0]);
                return 1;
            }
            shards.push_back(ep);
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (shards.empty()) {
        const char *env = std::getenv("KV_SHARDS");
        if (env) {
            std::string spec = env;
            size_t start = 0;
            while (start < spec.size()) {
                size_t comma = spec.find(',', start);
                std::string part = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                ShardEndpoint ep;
                if (!parse_host_port(part, ep)) {
                    fprintf(stderr, "invalid KV_SHARDS endpoint\n");
                    return 1;
                }
                shards.push_back(ep);
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        }
    }

    if (shards.empty()) {
        fprintf(stderr, "at least one --shard HOST:PORT is required\n");
        usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "proxy on port %u with %zu shards\n", port, shards.size());

    auto on_request = [&](const std::vector<std::string> &cmd, Buffer &out) {
        if ((cmd.size() == 1 || cmd.size() == 2) && cmd[0] == "ping") {
            return out_str(out, cmd.size() == 2 ? cmd[1] : "PONG");
        }
        if (is_scatter_gather_command(cmd)) {
            std::vector<Buffer> bodies;
            for (const ShardEndpoint &ep : shards) {
                Buffer framed;
                if (!tcp_rpc(ep.host, ep.port, cmd, framed)) {
                    return out_err(out, ERR_UNKNOWN, "shard unreachable");
                }
                Buffer body;
                if (!extract_response_body(framed, body)) {
                    return out_err(out, ERR_UNKNOWN, "bad shard response");
                }
                bodies.push_back(std::move(body));
            }
            if (!merge_key_arrays(bodies, out)) {
                return out_err(out, ERR_UNKNOWN, "failed to merge keys");
            }
            return;
        }

        auto key = routing_key_for_command(cmd);
        if (!key) {
            return out_err(out, ERR_UNKNOWN, "cannot route command");
        }
        size_t sid = shard_for_key(*key, shards.size());
        const ShardEndpoint &ep = shards[sid];
        Buffer framed;
        if (!tcp_rpc(ep.host, ep.port, cmd, framed)) {
            return out_err(out, ERR_UNKNOWN, "shard unreachable");
        }
        Buffer body;
        if (!extract_response_body(framed, body)) {
            return out_err(out, ERR_UNKNOWN, "bad shard response");
        }
        out.insert(out.end(), body.begin(), body.end());
    };

    run_tcp_server(port, on_request, nullptr, nullptr);
    return 0;
}
