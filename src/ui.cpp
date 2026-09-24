#include <FL/Fl.H> // fltk libs
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Box.H>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <cstring>
#include <cassert>

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = ::write(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = ::read(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int connect_server() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6379);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static int send_req(int fd, const std::vector<std::string> &cmd) {
    const size_t k_max_msg = 4096;
    uint32_t len = 4;
    for (const auto &s : cmd) len += 4 + (uint32_t)s.size();
    if (len > k_max_msg) return -1;
    char wbuf[4 + k_max_msg];
    std::memcpy(&wbuf[0], &len, 4);
    uint32_t n = (uint32_t)cmd.size();
    std::memcpy(&wbuf[4], &n, 4);
    size_t cur = 8;
    for (const auto &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        std::memcpy(&wbuf[cur], &p, 4);
        std::memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf, 4 + len);
}

static std::string read_res(int fd) {
    const size_t k_max_msg = 4096;
    char rbuf[4 + k_max_msg];
    if (read_full(fd, rbuf, 4) != 0) return "read header error";
    uint32_t len = 0;
    std::memcpy(&len, rbuf, 4);
    if (len > k_max_msg) return "response too long";
    if (read_full(fd, &rbuf[4], len) != 0) return "read body error";
    const unsigned char *data = (unsigned char *)&rbuf[4];
    if (len < 1) return "bad response";
    unsigned char tag = data[0];
    if (tag == 0) return "(nil)";
    if (tag == 1) {
        if (len < 1 + 8) return "bad response";
        int32_t code = 0; uint32_t slen = 0;
        std::memcpy(&code, &data[1], 4);
        std::memcpy(&slen, &data[1 + 4], 4);
        if (len < 1 + 8 + slen) return "bad response";
        return std::string("(err) ") + std::to_string(code) + " " + std::string((char*)&data[1+8], slen);
    }
    if (tag == 2) {
        if (len < 1 + 4) return "bad response";
        uint32_t slen = 0; std::memcpy(&slen, &data[1], 4);
        if (len < 1 + 4 + slen) return "bad response";
        return std::string((char*)&data[1+4], slen);
    }
    if (tag == 3) {
        if (len < 1 + 8) return "bad response";
        long long v = 0; std::memcpy(&v, &data[1], 8);
        return std::string("(int) ") + std::to_string(v);
    }
    if (tag == 4) {
        if (len < 1 + 8) return "bad response";
        double d = 0; std::memcpy(&d, &data[1], 8);
        return std::string("(dbl) ") + std::to_string(d);
    }
    if (tag == 5) {
        return "(arr) ...";
    }
    return "bad response";
}

struct UI : Fl_Window {
    Fl_Input *key;
    Fl_Input *val;
    Fl_Multiline_Output *out;
    Fl_Button *btn_set, *btn_get, *btn_update, *btn_del;
    // zset controls
    Fl_Input *zset;
    Fl_Input *zscore;
    Fl_Input *zmember;
    Fl_Input *zoffset;
    Fl_Input *zlimit;
    Fl_Button *btn_zadd, *btn_zrem, *btn_zscore, *btn_zquery;

    UI(int W, int H, const char *title) : Fl_Window(W, H, title) {
        begin();
        key = new Fl_Input(80, 20, 300, 30, "Key:");
        val = new Fl_Input(80, 60, 300, 30, "Value:");
        btn_set = new Fl_Button(20, 110, 80, 30, "Set");
        btn_get = new Fl_Button(110, 110, 80, 30, "Get");
        btn_update = new Fl_Button(200, 110, 80, 30, "Update");
        btn_del = new Fl_Button(290, 110, 80, 30, "Del");

        // zset inputs
        zset = new Fl_Input(80, 160, 300, 30, "ZSet:");
        zscore = new Fl_Input(80, 200, 300, 30, "Score:");
        zmember = new Fl_Input(80, 240, 300, 30, "Member:");
        new Fl_Box(80, 290, 140, 20, "Offset:");
        new Fl_Box(240, 290, 140, 20, "Limit:");
        zoffset = new Fl_Input(80, 320, 140, 30);
        zlimit = new Fl_Input(240, 320, 140, 30);

        btn_zadd = new Fl_Button(20, 370, 80, 30, "ZADD");
        btn_zrem = new Fl_Button(110, 370, 80, 30, "ZREM");
        btn_zscore = new Fl_Button(200, 370, 80, 30, "ZSCORE");
        btn_zquery = new Fl_Button(290, 370, 80, 30, "ZQUERY");

        out = new Fl_Multiline_Output(20, 430, 360, 180);
        end();
        btn_set->callback(cb_set, this);
        btn_get->callback(cb_get, this);
        btn_update->callback(cb_update, this);
        btn_del->callback(cb_del, this);
        btn_zadd->callback(cb_zadd, this);
        btn_zrem->callback(cb_zrem, this);
        btn_zscore->callback(cb_zscore, this);
        btn_zquery->callback(cb_zquery, this);
    }

    static void cb_set(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"set", ui->key->value(), ui->val->value()});
    }
    static void cb_get(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"get", ui->key->value()});
    }
    static void cb_update(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        // set is used for updates
        ui->do_cmd({"set", ui->key->value(), ui->val->value()});
    }
    static void cb_del(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"del", ui->key->value()});
    }

    static void cb_zadd(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"zadd", ui->zset->value(), ui->zscore->value(), ui->zmember->value()});
    }
    static void cb_zrem(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"zrem", ui->zset->value(), ui->zmember->value()});
    }
    static void cb_zscore(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"zscore", ui->zset->value(), ui->zmember->value()});
    }
    static void cb_zquery(Fl_Widget*, void* userdata) {
        UI *ui = (UI*)userdata;
        ui->do_cmd({"zquery", ui->zset->value(), ui->zscore->value(), ui->zmember->value(), ui->zoffset->value(), ui->zlimit->value()});
    }

    void do_cmd(const std::vector<std::string> &cmd) {
        int fd = connect_server();
        if (fd < 0) { out->value("connect failed"); return; }
        if (send_req(fd, cmd) != 0) { out->value("send failed"); ::close(fd); return; }
        std::string res = read_res(fd);
        out->value(res.c_str());
        ::close(fd);
    }
};

int main(int argc, char **argv) {
    UI ui(400, 580, "KV UI");
    ui.show(argc, argv);
    return Fl::run();
}
