#include "kv_store.hpp"

#include <cassert>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "common.h"
#include "hashtable.hpp"
#include "heap.hpp"
#include "list.h"
#include "thread_pool.hpp"
#include "zset.hpp"

static uint64_t get_monotonic_msec() {
    struct timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_nsec / 1000 / 1000;
}

namespace {

struct Entry {
    HNode node{};
    std::string key;
    size_t heap_idx = static_cast<size_t>(-1);
    uint32_t type = 0;
    std::string str;
    ZSet zset{};
};

static void entry_del_sync(Entry *ent) {
    if (ent->type == T_ZSET) {
        zset_clear(&ent->zset);
    }
    delete ent;
}

}  // namespace

struct KVStore::Impl {

    ~Impl();
    HMap db{};
    std::vector<HeapItem> heap;
    TheadPool thread_pool{};

    static Entry *entry_new(uint32_t type);

    void entry_set_ttl(Entry *ent, int64_t ttl_ms);
    void entry_del(Entry *ent);

    void heap_delete(size_t pos);
    void heap_upsert(size_t pos, HeapItem t);

    void do_get(std::vector<std::string> &cmd, Buffer &out);
    void do_set(std::vector<std::string> &cmd, Buffer &out);
    void do_del(std::vector<std::string> &cmd, Buffer &out);
    void do_expire(std::vector<std::string> &cmd, Buffer &out);
    void do_ttl(std::vector<std::string> &cmd, Buffer &out);
    void do_keys(Buffer &out);
    void do_zadd(std::vector<std::string> &cmd, Buffer &out);
    void do_zrem(std::vector<std::string> &cmd, Buffer &out);
    void do_zscore(std::vector<std::string> &cmd, Buffer &out);
    void do_zquery(std::vector<std::string> &cmd, Buffer &out);
    ZSet *expect_zset(std::string &s);
};

struct LookupKey {
    HNode node{};
    std::string key;
};

static bool entry_eq(HNode *node, HNode *key) {
    auto *ent = container_of(node, Entry, node);
    auto *keydata = container_of(key, LookupKey, node);
    return ent->key == keydata->key;
}

KVStore::Impl::~Impl() {
    thread_pool_destroy(&thread_pool);
    for (HTab *table : {&db.newer, &db.older}) {
        if (!table->tab) {
            continue;
        }
        for (size_t i = 0; i <= table->mask; ++i) {
            HNode *node = table->tab[i];
            while (node) {
                HNode *next = node->next;
                entry_del_sync(container_of(node, Entry, node));
                node = next;
            }
        }
    }
    hm_clear(&db);
}

KVStore::KVStore() : impl_(std::make_unique<Impl>()) {}
KVStore::~KVStore() = default;

void KVStore::init(size_t thread_pool_threads) {
    thread_pool_init(&impl_->thread_pool, thread_pool_threads);
}

static void entry_del_func(void *arg) {
    entry_del_sync(static_cast<Entry *>(arg));
}

Entry *KVStore::Impl::entry_new(uint32_t type) {
    auto *ent = new Entry();
    ent->type = type;
    return ent;
}

void KVStore::Impl::entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);
    size_t set_size = (ent->type == T_ZSET) ? hm_size(&ent->zset.hmap) : 0;
    constexpr size_t k_large_container_size = 1000;
    if (set_size > k_large_container_size) {
        thread_pool_queue(&thread_pool, &entry_del_func, ent);
    } else {
        entry_del_sync(ent);
    }
}

void KVStore::Impl::heap_delete(size_t pos) {
    heap[pos] = heap.back();
    heap.pop_back();
    if (pos < heap.size()) {
        heap_update(heap.data(), pos, heap.size());
    }
}

void KVStore::Impl::heap_upsert(size_t pos, HeapItem t) {
    if (pos < heap.size()) {
        heap[pos] = t;
    } else {
        pos = heap.size();
        heap.push_back(t);
    }
    heap_update(heap.data(), pos, heap.size());
}

void KVStore::Impl::entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != static_cast<size_t>(-1)) {
        heap_delete(ent->heap_idx);
        ent->heap_idx = static_cast<size_t>(-1);
    } else if (ttl_ms >= 0) {
        uint64_t expire_at = get_monotonic_msec() + static_cast<uint64_t>(ttl_ms);
        HeapItem item = {expire_at, &ent->heap_idx};
        heap_upsert(ent->heap_idx, item);
    }
}

static bool str2int(const std::string &s, int64_t &out) {
    if (s.empty()) {
        return false;
    }
    auto result = std::from_chars(s.data(), s.data() + s.size(), out);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}

static bool str2dbl(const std::string &s, double &out) {
    char *endp = nullptr;
    errno = 0;
    out = strtod(s.c_str(), &endp);
    return !s.empty() && endp != s.c_str() && endp == s.c_str() + s.size() &&
           errno != ERANGE && !std::isnan(out);
}

void KVStore::Impl::do_get(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *node = hm_lookup(&db, &key.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }
    Entry *ent = container_of(node, Entry, node);
    if (ent->type != T_STR) {
        return out_err(out, ERR_BAD_TYP, "not a string value");
    }
    out_str(out, ent->str);
}

void KVStore::Impl::do_set(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *node = hm_lookup(&db, &key.node, &entry_eq);
    Entry *ent = nullptr;
    if (node) {
        ent = container_of(node, Entry, node);
        if (ent->type != T_STR) {
            return out_err(out, ERR_BAD_TYP, "a non-string value exists");
        }
        ent->str.swap(cmd[2]);
    } else {
        ent = entry_new(T_STR);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->str.swap(cmd[2]);
        hm_insert(&db, &ent->node);
    }
    entry_set_ttl(ent, -1);
    out_str(out, "OK");
}

void KVStore::Impl::do_del(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *node = hm_delete(&db, &key.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    out_int(out, node ? 1 : 0);
}

void KVStore::Impl::do_expire(std::vector<std::string> &cmd, Buffer &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) {
        return out_err(out, ERR_BAD_ARG, "expect int64");
    }
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *node = hm_lookup(&db, &key.node, &entry_eq);
    if (node) {
        if (ttl_ms <= 0) {
            hm_delete(&db, &key.node, &entry_eq);
            entry_del(container_of(node, Entry, node));
        } else {
            entry_set_ttl(container_of(node, Entry, node), ttl_ms);
        }
    }
    out_int(out, node ? 1 : 0);
}

void KVStore::Impl::do_ttl(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *node = hm_lookup(&db, &key.node, &entry_eq);
    if (!node) {
        return out_int(out, -2);
    }
    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx == static_cast<size_t>(-1)) {
        return out_int(out, -1);
    }
    uint64_t expire_at = heap[ent->heap_idx].val;
    uint64_t now_ms = get_monotonic_msec();
    out_int(out, expire_at > now_ms ? static_cast<int64_t>(expire_at - now_ms) : 0);
}

static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *static_cast<Buffer *>(arg);
    const std::string &key = container_of(node, Entry, node)->key;
    out_str(out, key.data(), key.size());
    return true;
}

void KVStore::Impl::do_keys(Buffer &out) {
    out_arr(out, static_cast<uint32_t>(hm_size(&db)));
    hm_foreach(&db, &cb_keys, &out);
}

void KVStore::Impl::do_zadd(std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_BAD_ARG, "expect float");
    }
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *hnode = hm_lookup(&db, &key.node, &entry_eq);
    Entry *ent = nullptr;
    if (!hnode) {
        ent = entry_new(T_ZSET);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        hm_insert(&db, &ent->node);
    } else {
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_BAD_TYP, "expect zset");
        }
    }
    const std::string &name = cmd[3];
    bool added = zset_insert(&ent->zset, name.data(), name.size(), score);
    out_int(out, static_cast<int64_t>(added));
}

static const ZSet k_empty_zset{};

ZSet *KVStore::Impl::expect_zset(std::string &s) {
    LookupKey key;
    key.key.swap(s);
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
    HNode *hnode = hm_lookup(&db, &key.node, &entry_eq);
    if (!hnode) {
        return const_cast<ZSet *>(&k_empty_zset);
    }
    Entry *ent = container_of(hnode, Entry, node);
    return ent->type == T_ZSET ? &ent->zset : nullptr;
}

void KVStore::Impl::do_zrem(std::vector<std::string> &cmd, Buffer &out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }
    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name.data(), name.size());
    if (znode) {
        zset_delete(zset, znode);
    }
    out_int(out, znode ? 1 : 0);
}

void KVStore::Impl::do_zscore(std::vector<std::string> &cmd, Buffer &out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }
    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name.data(), name.size());
    if (znode) {
        out_dbl(out, znode->score);
    } else {
        out_nil(out);
    }
}

void KVStore::Impl::do_zquery(std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_BAD_ARG, "expect fp number");
    }
    const std::string &name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
        return out_err(out, ERR_BAD_ARG, "expect int");
    }
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }
    if (limit <= 0) {
        return out_arr(out, 0);
    }
    ZNode *znode = zset_seekge(zset, score, name.data(), name.size());
    znode = znode_offset(znode, offset);
    size_t ctx = out_begin_arr(out);
    int64_t n = 0;
    while (znode && n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        znode = znode_offset(znode, +1);
        n += 2;
    }
    out_end_arr(out, ctx, static_cast<uint32_t>(n));
}

void KVStore::execute(const std::vector<std::string> &cmd_in, Buffer &out) {
    const uint64_t now = get_monotonic_msec();
    while (!impl_->heap.empty() && impl_->heap[0].val <= now) {
        process_timers(now);
    }
    std::vector<std::string> cmd = cmd_in;
    Impl &db = *impl_;
    if ((cmd.size() == 1 || cmd.size() == 2) && cmd[0] == "ping") {
        out_str(out, cmd.size() == 2 ? cmd[1] : "PONG");
    } else if (cmd.size() == 2 && (cmd[0] == "exists" || cmd[0] == "type" ||
                                  cmd[0] == "persist" || cmd[0] == "incr")) {
        LookupKey key;
        key.key = cmd[1];
        key.node.hcode = str_hash(reinterpret_cast<const uint8_t *>(key.key.data()), key.key.size());
        HNode *node = hm_lookup(&db.db, &key.node, &entry_eq);
        Entry *ent = node ? container_of(node, Entry, node) : nullptr;
        if (cmd[0] == "exists") {
            out_int(out, ent ? 1 : 0);
        } else if (cmd[0] == "type") {
            out_str(out, !ent ? "none" : ent->type == T_STR ? "string" : "zset");
        } else if (cmd[0] == "persist") {
            bool changed = ent && ent->heap_idx != static_cast<size_t>(-1);
            if (changed) {
                db.entry_set_ttl(ent, -1);
            }
            out_int(out, changed ? 1 : 0);
        } else {
            int64_t value = 0;
            if (ent && ent->type != T_STR) {
                return out_err(out, ERR_BAD_TYP, "not a string value");
            }
            if (ent && (!str2int(ent->str, value) || value == std::numeric_limits<int64_t>::max())) {
                return out_err(out, ERR_BAD_ARG, "value is not an integer or increment would overflow");
            }
            if (!ent) {
                ent = Impl::entry_new(T_STR);
                ent->key = key.key;
                ent->node.hcode = key.node.hcode;
                hm_insert(&db.db, &ent->node);
            }
            ent->str = std::to_string(++value);
            out_int(out, value);
        }
    } else if (cmd.size() == 2 && cmd[0] == "get") {
        db.do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        db.do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        db.do_del(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "pexpire") {
        db.do_expire(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "pttl") {
        db.do_ttl(cmd, out);
    } else if (cmd.size() == 1 && cmd[0] == "keys") {
        db.do_keys(out);
    } else if (cmd.size() == 4 && cmd[0] == "zadd") {
        db.do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "zrem") {
        db.do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "zscore") {
        db.do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd[0] == "zquery") {
        db.do_zquery(cmd, out);
    } else {
        out_err(out, ERR_UNKNOWN, "unknown command.");
    }
}

uint32_t KVStore::next_timer_ms(uint64_t now_ms) const {
    uint64_t next_ms = static_cast<uint64_t>(-1);
    const auto &heap = impl_->heap;
    if (!heap.empty() && heap[0].val < next_ms) {
        next_ms = heap[0].val;
    }
    if (next_ms == static_cast<uint64_t>(-1)) {
        return static_cast<uint32_t>(-1);
    }
    if (next_ms <= now_ms) {
        return 0;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(next_ms - now_ms, INT32_MAX));
}

static bool hnode_same(HNode *node, HNode *key) {
    return node == key;
}

void KVStore::process_timers(uint64_t now_ms) {
  Impl &db = *impl_;
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    while (!db.heap.empty() && db.heap[0].val <= now_ms) {
        auto *ent = container_of(db.heap[0].ref, Entry, heap_idx);
        HNode *node = hm_delete(&db.db, &ent->node, &hnode_same);
        assert(node == &ent->node);
        db.entry_del(ent);
        if (nworks++ >= k_max_works) {
            break;
        }
    }
}
