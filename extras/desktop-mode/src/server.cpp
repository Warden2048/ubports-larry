// larry-scrcpy-server: a scrcpy (v5.x protocol) compatible server for Ubuntu Touch / Halium.
//
// The stock scrcpy client pushes its Java server and runs
//   adb shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server <ver> key=value...
// On this phone /usr/local/bin/app_process is a wrapper that execs this program with the same arguments.
//
// Video: Mir screencast (USC socket). With new_display=WxH the capture region lies right of the phone
// screen, which makes Mir create a virtual output that Lomiri uses as a second monitor (aethercast's trick).
// Frames are encoded by the Qualcomm hardware encoder through libhybris' MediaCodecSource, zero-copy
// (ANativeWindowBuffer metadata), as aethercast does.
// Build (cross, aarch64; R = a sysroot with the 24.04-2.x libmir1client-dev, libmir1core-dev,
// libmir1common-dev and libhybris media headers/libs):
//   aarch64-linux-gnu-g++ -O2 -std=c++17 -o larry-scrcpy-server server.cpp -I$R/usr/include/mir1client \
//     -I$R/usr/include/mir1core -I$R/usr/include/mir1common -I$R/usr/include -L$R/usr/lib/aarch64-linux-gnu \
//     -Wl,--allow-shlib-undefined -Wl,--unresolved-symbols=ignore-in-shared-libs -lmir1client -lmedia -lpthread
// Input: scrcpy UHID messages (--keyboard=uhid --mouse=uhid) go to /dev/uhid -> real HID devices for Lomiri.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/uhid.h>
#include <linux/uinput.h>
#include <cmath>

#include <mir_toolkit/mir_client_library.h>
#include <mir_toolkit/mir_screencast.h>
#include <mir_toolkit/mir_buffer_stream.h>

extern "C" {
#include <hybris/media/media_buffer_layer.h>
#include <hybris/media/media_codec_source_layer.h>
#include <hybris/media/media_message_layer.h>
#include <hybris/media/media_meta_data_layer.h>
}

// ---------------------------------------------------------------- logging
static int g_verbose = 1;
static FILE *g_logfile = nullptr;
static void logf(const char *lvl, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[larry-scrcpy] %s: ", lvl); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    if (g_logfile) {
        va_start(ap, fmt);
        fprintf(g_logfile, "%s: ", lvl); vfprintf(g_logfile, fmt, ap); fputc('\n', g_logfile); fflush(g_logfile);
        va_end(ap);
    }
}
#define LOGI(...) do { if (g_verbose) logf("INFO", __VA_ARGS__); } while (0)
#define LOGE(...) logf("ERROR", __VA_ARGS__)

// ---------------------------------------------------------------- options
struct Options {
    uint32_t scid = 0xffffffff;
    bool video = true, audio = true, control = true;
    bool tunnel_forward = false;
    int bit_rate = 8000000;
    int max_fps = 60;
    int max_size = 0;
    bool new_display = false;
    int nd_width = 1920, nd_height = 1080;
    bool send_device_meta = true, send_stream_meta = true, send_frame_meta = true, send_dummy_byte = true;
    std::string video_codec = "h264";
    std::string codec_options;   // scrcpy --video-codec-options: key[:type]=value,...
};

static bool parse_bool(const std::string &v) { return v == "true"; }

static Options parse_options(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;  // "/", class name, version
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "scid") o.scid = (uint32_t)strtoul(v.c_str(), nullptr, 16);
        else if (k == "video") o.video = parse_bool(v);
        else if (k == "audio") o.audio = parse_bool(v);
        else if (k == "control") o.control = parse_bool(v);
        else if (k == "tunnel_forward") o.tunnel_forward = parse_bool(v);
        else if (k == "video_bit_rate") o.bit_rate = atoi(v.c_str());
        else if (k == "max_fps") o.max_fps = (int)atof(v.c_str());
        else if (k == "max_size") o.max_size = atoi(v.c_str());
        else if (k == "video_codec") o.video_codec = v;
        else if (k == "video_codec_options") o.codec_options = v;
        else if (k == "send_device_meta") o.send_device_meta = parse_bool(v);
        else if (k == "send_stream_meta") o.send_stream_meta = parse_bool(v);
        else if (k == "send_frame_meta") o.send_frame_meta = parse_bool(v);
        else if (k == "send_dummy_byte") o.send_dummy_byte = parse_bool(v);
        else if (k == "raw_stream" && parse_bool(v)) { o.send_device_meta = o.send_stream_meta = o.send_frame_meta = o.send_dummy_byte = false; }
        else if (k == "log_level") g_verbose = (v != "warn" && v != "error");
        else if (k == "new_display") {
            o.new_display = true;
            int w = 0, h = 0;
            if (sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { o.nd_width = w; o.nd_height = h; }
        }
    }
    if (o.max_fps <= 0) o.max_fps = 60;
    if (o.bit_rate <= 0) o.bit_rate = 8000000;
    return o;
}

// ---------------------------------------------------------------- phone-side config (~/.config/larry-scrcpy.conf)
struct Config { double scroll_multiplier = 3.0; int max_in_flight = 2; double operating_rate = 0; double desktop_refresh = 60; int capture_fps = 0; int flat_mouse = 0; };
static Config g_cfg;
static bool g_hevc = false;   // scrcpy --video-codec=h265
static void load_config() {
    const char *home = getenv("HOME");
    std::string path = std::string(home ? home : "/home/phablet") + "/.config/larry-scrcpy.conf";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char k[64]; double v;
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %lf", k, &v) != 2) continue;
        if (!strcmp(k, "scroll_multiplier") && v > 0) g_cfg.scroll_multiplier = v;
        else if (!strcmp(k, "max_in_flight") && v >= 1 && v <= 2) g_cfg.max_in_flight = (int)v;
        else if (!strcmp(k, "operating_rate") && v >= 0) g_cfg.operating_rate = v;
        else if (!strcmp(k, "desktop_refresh") && v >= 0) g_cfg.desktop_refresh = v;
        else if (!strcmp(k, "capture_fps") && v >= 0 && v <= 240) g_cfg.capture_fps = (int)v;
        else if (!strcmp(k, "flat_mouse")) g_cfg.flat_mouse = v != 0;
    }
    fclose(f);
}

// ---------------------------------------------------------------- sockets
static int g_video_fd = -1, g_audio_fd = -1, g_control_fd = -1;
static std::atomic<bool> g_stop{false};

static bool write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n; len -= (size_t)n;
    }
    return true;
}
static bool read_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0) return false;
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n; len -= (size_t)n;
    }
    return true;
}
static socklen_t abstract_addr(const std::string &name, sockaddr_un *a) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name.data(), name.size());  // leading NUL = abstract namespace
    return (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size());
}
static int connect_abstract(const std::string &name) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a; socklen_t l = abstract_addr(name, &a);
    if (connect(fd, (sockaddr *)&a, l) < 0) { LOGE("connect %s: %s", name.c_str(), strerror(errno)); close(fd); return -1; }
    return fd;
}

static bool open_connection(const Options &o) {
    char name[64];
    if (o.scid == 0xffffffff) snprintf(name, sizeof(name), "scrcpy");
    else snprintf(name, sizeof(name), "scrcpy_%08x", o.scid);
    std::vector<int *> order;
    if (o.video) order.push_back(&g_video_fd);
    if (o.audio) order.push_back(&g_audio_fd);
    if (o.control) order.push_back(&g_control_fd);
    if (o.tunnel_forward) {
        int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a; socklen_t l = abstract_addr(name, &a);
        if (bind(srv, (sockaddr *)&a, l) < 0 || listen(srv, 4) < 0) { LOGE("listen %s: %s", name, strerror(errno)); return false; }
        bool dummy = o.send_dummy_byte;
        for (int *fdp : order) {
            *fdp = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
            if (*fdp < 0) { LOGE("accept: %s", strerror(errno)); return false; }
            if (dummy) { uint8_t z = 0; write_all(*fdp, &z, 1); dummy = false; }
        }
        close(srv);
    } else {
        for (int *fdp : order) { if ((*fdp = connect_abstract(name)) < 0) return false; }
    }
    if (o.send_device_meta) {
        char meta[64] = {0};
        snprintf(meta, sizeof(meta), "OnePlus Nord N30 (Ubuntu Touch)");
        write_all(*order[0], meta, sizeof(meta));
    }
    return true;
}

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---------------------------------------------------------------- UHID input
static int g_uhid_fd = -1;
static std::map<uint16_t, int> g_uhid_devs;  // scrcpy id -> fd (one /dev/uhid fd per device)

struct Stats { std::mutex m; long frames_in = 0, frames_out = 0, bytes = 0, keyframes = 0, changed = 0, captures = 0, keys = 0, mouse = 0; double swap_ms = 0, enc_ms = 0, send_ms = 0, wait_ms = 0; };
static Stats g_stats;
static void configure_mouse_devices();
static void uhid_create(uint16_t id, uint16_t vid, uint16_t pid, const std::string &name, const std::vector<uint8_t> &desc) {
    int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOGE("open /dev/uhid: %s", strerror(errno)); return; }
    struct uhid_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    const char *kind = id == 1 ? "keyboard" : id == 2 ? "mouse" : "gamepad";
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "scrcpy %s", name.empty() ? kind : name.c_str());
    size_t n = desc.size() < HID_MAX_DESCRIPTOR_SIZE ? desc.size() : HID_MAX_DESCRIPTOR_SIZE;
    memcpy(ev.u.create2.rd_data, desc.data(), n);
    ev.u.create2.rd_size = (uint16_t)n;
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = vid ? vid : 0x18d1;
    ev.u.create2.product = pid ? pid : 0x4ee1;
    if (write(fd, &ev, sizeof(ev)) < 0) { LOGE("UHID_CREATE2: %s", strerror(errno)); close(fd); return; }
    auto it = g_uhid_devs.find(id);
    if (it != g_uhid_devs.end()) close(it->second);
    g_uhid_devs[id] = fd;
    // The kernel talks back on the uhid fd (start/open/close, LED output reports, get/set-report
    // requests). Unanswered get/set-report requests stall the HID device for seconds at a time,
    // so service them.
    std::thread([fd, id] {
        struct uhid_event e;
        int outputs = 0;
        while (true) {
            ssize_t r = read(fd, &e, sizeof(e));
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            switch (e.type) {
            case UHID_OPEN: LOGI("uhid %u opened by the compositor", id); break;
            case UHID_CLOSE: LOGI("uhid %u closed by the compositor", id); break;
            case UHID_OUTPUT: if (outputs++ < 3) LOGI("uhid %u output report (%u bytes, e.g. keyboard LEDs)", id, e.u.output.size); break;
            case UHID_GET_REPORT: {
                struct uhid_event a; memset(&a, 0, sizeof(a)); a.type = UHID_GET_REPORT_REPLY;
                a.u.get_report_reply.id = e.u.get_report.id; a.u.get_report_reply.err = EIO;
                if (write(fd, &a, sizeof(a)) < 0) {}
                LOGI("uhid %u get-report answered", id); break;
            }
            case UHID_SET_REPORT: {
                struct uhid_event a; memset(&a, 0, sizeof(a)); a.type = UHID_SET_REPORT_REPLY;
                a.u.set_report_reply.id = e.u.set_report.id; a.u.set_report_reply.err = 0;
                if (write(fd, &a, sizeof(a)) < 0) {}
                LOGI("uhid %u set-report answered", id); break;
            }
            default: break;
            }
        }
    }).detach();
    LOGI("uhid device %u created (scrcpy %s, %zu-byte descriptor)", id, name.empty() ? kind : name.c_str(), n);
    if (id == 2 && g_cfg.flat_mouse) std::thread(configure_mouse_devices).detach();
}
static double g_hid_wheel_acc[2];
static void uhid_input(uint16_t id, const std::vector<uint8_t> &data_in) {
    auto it = g_uhid_devs.find(id);
    if (it == g_uhid_devs.end()) return;
    std::vector<uint8_t> data = data_in;
    if (id == 2 && data.size() == 5 && g_cfg.scroll_multiplier != 1.0) {   // scrcpy HID mouse: buttons, x, y, wheel, h-wheel
        for (int k = 0; k < 2; k++) {
            int8_t w = (int8_t)data[3 + k];
            if (!w) continue;
            g_hid_wheel_acc[k] += w * g_cfg.scroll_multiplier;
            int n = (int)g_hid_wheel_acc[k]; if (n > 127) n = 127; if (n < -127) n = -127;
            g_hid_wheel_acc[k] -= n; data[3 + k] = (uint8_t)(int8_t)n;
        }
    }
    struct uhid_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    size_t n = data.size() < UHID_DATA_MAX ? data.size() : UHID_DATA_MAX;
    memcpy(ev.u.input2.data, data.data(), n);
    ev.u.input2.size = (uint16_t)n;
    if (write(it->second, &ev, sizeof(ev)) < 0) LOGE("UHID_INPUT2: %s", strerror(errno));
    std::lock_guard<std::mutex> l(g_stats.m); (id == 1 ? g_stats.keys : g_stats.mouse)++;
}
static void uhid_destroy(uint16_t id) {
    auto it = g_uhid_devs.find(id);
    if (it == g_uhid_devs.end()) return;
    struct uhid_event ev; memset(&ev, 0, sizeof(ev)); ev.type = UHID_DESTROY;
    write(it->second, &ev, sizeof(ev));
    close(it->second); g_uhid_devs.erase(it);
}

// ---------------------------------------------------------------- absolute pointer (scrcpy --mouse=sdk)
// Mir turns an absolute device's position into a *movement* (new - previous position, scaled to the
// bounding box of the powered-on outputs), and Lomiri moves its own cursor by that movement, clamped
// to its screens. So: (1) positions are scaled 1:1 to Mir pixels using the live bounding box, which
// gives exact, unaccelerated movement, and (2) Lomiri's cursor is "homed" to a known corner of the
// target screen (top-right; pushing there triggers no shell gesture) with a small relative device,
// after which absolute positions line up exactly.
struct PointerMap { int rx = 0, ry = 0, rw = 1, rh = 1; int bw = 1, bh = 1; int gen = 0; };
static PointerMap g_pmap;
static std::mutex g_pmap_m;
static int g_uinput = -1, g_uinput_rel = -1;
static int g_buttons = 0;          // Android button mask currently held
static double g_wheel_acc_v = 0, g_wheel_acc_h = 0;
static int g_homed_gen = -1;
static std::chrono::steady_clock::time_point g_last_ptr_evt;
static constexpr int kAbsMax = 65535;

static void emit_fd(int fd, int type, int code, int value) {
    struct input_event ev; memset(&ev, 0, sizeof(ev));
    ev.type = (uint16_t)type; ev.code = (uint16_t)code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {}
}
static void emit(int type, int code, int value) { emit_fd(g_uinput, type, code, value); }
static bool pointer_open() {
    if (g_uinput >= 0) return true;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { LOGE("open /dev/uinput: %s", strerror(errno)); return false; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_ABS); ioctl(fd, UI_SET_EVBIT, EV_REL); ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) ioctl(fd, UI_SET_KEYBIT, b);
    for (int r : {REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) ioctl(fd, UI_SET_RELBIT, r);
    ioctl(fd, UI_SET_ABSBIT, ABS_X); ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    struct uinput_user_dev ud; memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "scrcpy absolute pointer");
    ud.id.bustype = BUS_VIRTUAL; ud.id.vendor = 0x18d1; ud.id.product = 0x4ee2; ud.id.version = 1;
    ud.absmax[ABS_X] = kAbsMax; ud.absmax[ABS_Y] = kAbsMax;
    if (write(fd, &ud, sizeof(ud)) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { LOGE("uinput create: %s", strerror(errno)); close(fd); return false; }
    g_uinput = fd;
    // relative helper, only used to push Lomiri's cursor into a corner
    int rf = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (rf >= 0) {
        ioctl(rf, UI_SET_EVBIT, EV_KEY); ioctl(rf, UI_SET_EVBIT, EV_REL); ioctl(rf, UI_SET_EVBIT, EV_SYN);
        ioctl(rf, UI_SET_KEYBIT, BTN_LEFT); ioctl(rf, UI_SET_RELBIT, REL_X); ioctl(rf, UI_SET_RELBIT, REL_Y);
        struct uinput_user_dev rd; memset(&rd, 0, sizeof(rd));
        snprintf(rd.name, UINPUT_MAX_NAME_SIZE, "scrcpy pointer sync");
        rd.id.bustype = BUS_VIRTUAL; rd.id.vendor = 0x18d1; rd.id.product = 0x4ee3; rd.id.version = 1;
        if (write(rf, &rd, sizeof(rd)) < 0 || ioctl(rf, UI_DEV_CREATE) < 0) { LOGE("uinput (sync) create: %s", strerror(errno)); close(rf); rf = -1; }
    }
    g_uinput_rel = rf;
    LOGI("absolute pointer created (scroll x%.1f, sync device %s)", g_cfg.scroll_multiplier, rf >= 0 ? "ok" : "missing");
    return true;
}
// Mir pixel position (relative to the bounding box) -> raw axis value that libinput maps back to
// exactly that pixel: libinput gives raw * box / 65536 and Mir truncates.
static void abs_emit_px(const PointerMap &m, double px, double py) {
    px = std::max(0.0, std::min(px, (double)m.bw - 1)); py = std::max(0.0, std::min(py, (double)m.bh - 1));
    long ax = (long)std::ceil(std::floor(px) * 65536.0 / m.bw), ay = (long)std::ceil(std::floor(py) * 65536.0 / m.bh);
    emit(EV_ABS, ABS_X, (int)std::min<long>(ax, kAbsMax));
    emit(EV_ABS, ABS_Y, (int)std::min<long>(ay, kAbsMax));
}
static void rel_step(int dx, int dy) {
    if (dx) emit_fd(g_uinput_rel, EV_REL, REL_X, dx);
    if (dy) emit_fd(g_uinput_rel, EV_REL, REL_Y, dy);
    emit_fd(g_uinput_rel, EV_SYN, SYN_REPORT, 0);
    std::this_thread::sleep_for(std::chrono::microseconds(1500));
}
// Put Lomiri's cursor at the top-right pixel of the captured screen and the absolute device's last
// position on the same pixel, so the next absolute move is exact.
static void pointer_home(const PointerMap &m) {
    if (g_uinput_rel < 0) return;
    abs_emit_px(m, m.rx + m.rw - 1, m.ry);
    emit(EV_SYN, SYN_REPORT, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    for (int i = 0; i < 6; i++) rel_step(0, -600);     // up: clamps to the top of the current screen
    for (int i = 0; i < 60; i++) rel_step(70, 0);      // right along the top edge, into the right-most screen's corner
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    g_homed_gen = m.gen;
    LOGI("pointer synced (map gen %d)", m.gen);
}
static void pointer_move(int x, int y, int vw, int vh) {
    PointerMap m; { std::lock_guard<std::mutex> l(g_pmap_m); m = g_pmap; }
    if (vw <= 0 || vh <= 0 || m.bw <= 1 || m.bh <= 1) return;
    auto now = std::chrono::steady_clock::now();
    bool idle = now - g_last_ptr_evt > std::chrono::seconds(3);
    g_last_ptr_evt = now;
    if (g_buttons == 0 && (g_homed_gen != m.gen || idle)) pointer_home(m);
    abs_emit_px(m, m.rx + (double)x * m.rw / vw, m.ry + (double)y * m.rh / vh);
}
static void pointer_buttons(int mask) {
    static const int map[][2] = {{1, BTN_LEFT}, {2, BTN_RIGHT}, {4, BTN_MIDDLE}, {8, BTN_SIDE}, {16, BTN_EXTRA}};
    for (auto &mb : map)
        if ((mask ^ g_buttons) & mb[0]) emit(EV_KEY, mb[1], (mask & mb[0]) ? 1 : 0);
    g_buttons = mask;
}
// scrcpy INJECT_TOUCH_EVENT
static void on_touch(int action, int64_t pointer_id, int x, int y, int vw, int vh, int buttons) {
    if (!pointer_open()) return;
    pointer_move(x, y, vw, vh);
    int mask;
    if (pointer_id == -1) mask = (action == 1 /*UP*/) ? 0 : buttons;          // the PC mouse
    else mask = (action == 1 || action == 3 /*UP, CANCEL*/) ? 0 : 1;           // finger emulation -> left button
    pointer_buttons(mask);
    emit(EV_SYN, SYN_REPORT, 0);
}
// scrcpy INJECT_SCROLL_EVENT (h/v in notches, may be fractional)
static void on_scroll(int x, int y, int vw, int vh, float h, float v) {
    if (!pointer_open()) return;
    pointer_move(x, y, vw, vh);
    double m = g_cfg.scroll_multiplier;
    if (v != 0) {
        emit(EV_REL, REL_WHEEL_HI_RES, (int)lround(v * 120 * m));
        g_wheel_acc_v += v * m; int n = (int)g_wheel_acc_v; if (n) { emit(EV_REL, REL_WHEEL, n); g_wheel_acc_v -= n; }
    }
    if (h != 0) {
        emit(EV_REL, REL_HWHEEL_HI_RES, (int)lround(h * 120 * m));
        g_wheel_acc_h += h * m; int n = (int)g_wheel_acc_h; if (n) { emit(EV_REL, REL_HWHEEL, n); g_wheel_acc_h -= n; }
    }
    emit(EV_SYN, SYN_REPORT, 0);
}
static int32_t be32s(const uint8_t *p) { return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]); }

// ---------------------------------------------------------------- control channel
static bool skip(int fd, size_t n) { std::vector<uint8_t> b(n); return n == 0 || read_all(fd, b.data(), n); }
static bool read_lenbuf(int fd, int size_bytes, std::vector<uint8_t> &out) {
    uint8_t lb[4]; if (!read_all(fd, lb, size_bytes)) return false;
    uint32_t len = 0; for (int i = 0; i < size_bytes; i++) len = len << 8 | lb[i];
    out.resize(len); return len == 0 || read_all(fd, out.data(), len);
}

static void control_loop() {
    int fd = g_control_fd;
    std::vector<uint8_t> s, d;
    while (!g_stop) {
        uint8_t type;
        if (!read_all(fd, &type, 1)) break;
        bool ok = true;
        switch (type) {
        case 0: ok = skip(fd, 1 + 4 + 4 + 4); break;                       // inject keycode
        case 1: ok = read_lenbuf(fd, 4, s); break;                         // inject text
        case 2: {                                                          // touch / mouse
            uint8_t b[1 + 8 + 12 + 2 + 4 + 4]; ok = read_all(fd, b, sizeof(b));
            if (ok) {
                int64_t pid = (int64_t)((uint64_t)(uint32_t)be32s(b + 1) << 32 | (uint32_t)be32s(b + 5));
                on_touch(b[0], pid, be32s(b + 9), be32s(b + 13), be16(b + 17), be16(b + 19), be32s(b + 27));
            }
            break;
        }
        case 3: {                                                          // scroll
            uint8_t b[12 + 2 + 2 + 4]; ok = read_all(fd, b, sizeof(b));
            if (ok) {
                float h = (int16_t)be16(b + 12) / 32768.0f * 16, v = (int16_t)be16(b + 14) / 32768.0f * 16;
                on_scroll(be32s(b), be32s(b + 4), be16(b + 8), be16(b + 10), h, v);
            }
            break;
        }
        case 4: ok = skip(fd, 1); break;                                   // back or screen on
        case 8: ok = skip(fd, 1); break;                                   // get clipboard
        case 9: ok = skip(fd, 8 + 1) && read_lenbuf(fd, 4, s); break;      // set clipboard
        case 10: ok = skip(fd, 1); break;                                  // set display power
        case 5: case 6: case 7: case 11: case 15: case 17: case 19: case 20: break;
        case 12: {                                                         // uhid create
            uint8_t h[6]; ok = read_all(fd, h, 6) && read_lenbuf(fd, 1, s) && read_lenbuf(fd, 2, d);
            if (ok) uhid_create(be16(h), be16(h + 2), be16(h + 4), std::string(s.begin(), s.end()), d);
            break;
        }
        case 13: {                                                         // uhid input
            uint8_t h[2]; ok = read_all(fd, h, 2) && read_lenbuf(fd, 2, d);
            if (ok) uhid_input(be16(h), d);
            break;
        }
        case 14: { uint8_t h[2]; ok = read_all(fd, h, 2); if (ok) uhid_destroy(be16(h)); break; }
        case 16: ok = read_lenbuf(fd, 1, s); break;                        // start app
        case 18: ok = skip(fd, 1); break;                                  // camera torch
        case 21: ok = skip(fd, 4); break;                                  // resize display
        case 22: ok = read_lenbuf(fd, 4, s); break;                        // scan file
        default: LOGE("unknown control message %u", type); ok = false;
        }
        if (!ok) break;
    }
    LOGI("control channel closed");
    g_stop = true;
}

// ---------------------------------------------------------------- encoder (libhybris MediaCodecSource)
static constexpr int32_t kOMXColorFormatAndroidOpaque = 0x7F000789;
static constexpr int32_t kOMXVideoControlRateVariable = 1;
static constexpr int kMetadataBufferTypeANWBuffer = 2;
enum { kErrNotConnected = -1001, kErrBufferTooSmall = -1009, kErrEOS = -1011 };
struct VideoNativeMetadata { int32_t eType; void *pBuffer; int nFenceFd; };

struct Encoder {
    MediaCodecSourceWrapper *codec = nullptr;
    std::mutex m;
    std::condition_variable cv;
    MirNativeBuffer *pending = nullptr;   // next frame for the encoder
    int64_t pending_ts = 0;
    int in_flight = 0;                    // frames handed to the encoder and not returned yet
    bool running = false;

    static int on_start(MediaMetaDataWrapper *, void *) { return 0; }
    static int on_stop(void *u) { auto *e = (Encoder *)u; std::lock_guard<std::mutex> l(e->m); e->running = false; e->cv.notify_all(); return 0; }
    static int on_pause(void *) { return 0; }
    static void on_returned(MediaBufferWrapper *b, void *u) {
        auto *e = (Encoder *)u;
        media_buffer_set_return_callback(b, nullptr, nullptr);
        media_buffer_destroy(b);
        std::lock_guard<std::mutex> l(e->m); e->in_flight--; e->cv.notify_all();
    }
    static int on_read(MediaBufferWrapper **out, void *u) {
        auto *e = (Encoder *)u;
        if (!out) return kErrBufferTooSmall;
        std::unique_lock<std::mutex> l(e->m);
        e->cv.wait(l, [e] { return e->pending || !e->running || g_stop; });
        if (!e->running || g_stop) return kErrEOS;
        auto *b = media_buffer_create(sizeof(VideoNativeMetadata));
        if (!b) return kErrEOS;
        auto *md = (VideoNativeMetadata *)media_buffer_get_data(b);
        memset(md, 0, sizeof(*md));
        md->eType = kMetadataBufferTypeANWBuffer; md->pBuffer = e->pending; md->nFenceFd = -1;
        media_buffer_set_return_callback(b, &Encoder::on_returned, e);
        media_buffer_ref(b);
        auto *meta = media_buffer_get_meta_data(b);
        media_meta_data_set_int64(meta, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_TIME), e->pending_ts);
        media_meta_data_release(meta);
        e->pending = nullptr; e->in_flight++;
        e->cv.notify_all();
        *out = b;
        return 0;
    }

    std::string options;
    bool use_operating_rate = true;
    bool use_profile = true;
    void apply_codec_options(MediaMessageWrapper *f) {
        media_message_set_int32(f, "vendor.qti-ext-enc-low-latency.enable", 1);
        media_message_set_int32(f, "latency", 1);
        media_message_set_int32(f, "low-latency", 1);              // Android 11+ MediaFormat.KEY_LOW_LATENCY
        if (use_operating_rate && g_cfg.operating_rate > 0) {   // ask the video engine to clock for this rate -> faster per-frame encode
            media_message_set_float(f, "operating-rate", (float)g_cfg.operating_rate);
            LOGI("codec option operating-rate = %.0f", g_cfg.operating_rate);
        }
        size_t pos = 0;
        while (pos < options.size()) {
            size_t end = options.find(',', pos); if (end == std::string::npos) end = options.size();
            std::string item = options.substr(pos, end - pos); pos = end + 1;
            size_t eq = item.find('='); if (eq == std::string::npos) continue;
            std::string key = item.substr(0, eq), val = item.substr(eq + 1), type = "int";
            size_t c = key.find(':'); if (c != std::string::npos) { type = key.substr(c + 1); key = key.substr(0, c); }
            if (type == "long") media_message_set_int64(f, key.c_str(), atoll(val.c_str()));
            else if (type == "float") media_message_set_float(f, key.c_str(), (float)atof(val.c_str()));
            else if (type == "string") media_message_set_string(f, key.c_str(), val.c_str(), -1);
            else media_message_set_int32(f, key.c_str(), atoi(val.c_str()));
            LOGI("codec option %s (%s) = %s", key.c_str(), type.c_str(), val.c_str());
        }
    }

    bool start(int w, int h, int fps, int bitrate) {
        auto *f = media_message_create();
        media_message_set_string(f, "mime", g_hevc ? "video/hevc" : "video/avc", 0);
        if (use_profile) {   // better picture per bit: AVC High @ 4.2 / HEVC Main @ 4.1 (1080p60)
            media_message_set_int32(f, "profile", g_hevc ? 1 : 8);
            media_message_set_int32(f, "level", g_hevc ? 0x1000 : 0x2000);
        }
        media_message_set_int32(f, "store-metadata-in-buffers", kMetadataBufferTypeANWBuffer);
        media_message_set_int32(f, "android._input-metadata-buffer-type", kMetadataBufferTypeANWBuffer);
        media_message_set_int32(f, "store-metadata-in-buffers-output", 0);
        media_message_set_int32(f, "android._store-metadata-in-buffers-output", 0);
        media_message_set_int32(f, "android._using-recorder", 1);
        media_message_set_int32(f, "width", w);
        media_message_set_int32(f, "height", h);
        media_message_set_int32(f, "stride", w);
        media_message_set_int32(f, "slice-height", h);
        media_message_set_int32(f, "color-format", kOMXColorFormatAndroidOpaque);
        media_message_set_int32(f, "bitrate", bitrate);
        media_message_set_int32(f, "bitrate-mode", kOMXVideoControlRateVariable);
        media_message_set_int32(f, "frame-rate", fps);
        media_message_set_int32(f, "i-frame-interval", 10);
        media_message_set_int32(f, "max-bframes", 0);
        media_message_set_int32(f, "prepend-sps-pps-to-idr-frames", 1);
        media_message_set_int32(f, "priority", 0);  // realtime
        apply_codec_options(f);
        auto *src = media_source_create();
        auto *sf = media_meta_data_create();
        media_meta_data_set_cstring(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_MIME), "video/raw");
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_COLOR_FORMAT), kOMXColorFormatAndroidOpaque);
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_WIDTH), w);
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_HEIGHT), h);
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_STRIDE), w);
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_SLICE_HEIGHT), h);
        media_meta_data_set_int32(sf, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_FRAMERATE), fps);
        media_source_set_format(src, sf);
        media_source_set_start_callback(src, &Encoder::on_start, this);
        media_source_set_stop_callback(src, &Encoder::on_stop, this);
        media_source_set_read_callback(src, &Encoder::on_read, this);
        media_source_set_pause_callback(src, &Encoder::on_pause, this);
        codec = media_codec_source_create(f, src, 0);
        if (!codec) { LOGE("media_codec_source_create failed"); return false; }
        running = true;
        if (!media_codec_source_start(codec)) { LOGE("media_codec_source_start failed"); running = false; return false; }
        LOGI("encoder started: %s %dx%d @ %d fps, %d bit/s%s", g_hevc ? "H.265" : "H.264", w, h, fps, bitrate, use_profile ? " (high-quality profile)" : "");
        return true;
    }

    // Hand a captured frame to the encoder; blocks until the encoder took the previous one.
    void submit(MirNativeBuffer *buf, int64_t ts) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [this] { return !pending || !running || g_stop; });
        pending = buf; pending_ts = ts; cv.notify_all();
    }
    // Wait until at most `max_in_flight` frames are still held by the encoder. The Mir stream has
    // kMirBuffers buffers, so a buffer handed back to Mir is only redrawn after that many swaps.
    void wait_slots(int max_in_flight) {
        std::unique_lock<std::mutex> l(m);
        cv.wait_for(l, std::chrono::milliseconds(200), [&] { return (!pending && in_flight <= max_in_flight) || !running || g_stop; });
    }
};


static bool nal_has_idr(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1))) {
            size_t h = p[i + 2] == 1 ? i + 3 : i + 4;
            if (h >= n) continue;
            if (g_hevc) { int t = (p[h] >> 1) & 0x3f; if (t >= 16 && t <= 21) return true; }   // IRAP pictures
            else if ((p[h] & 0x1f) == 5) return true;
        }
    }
    return false;
}

static int64_t now_us(const std::chrono::steady_clock::time_point &t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
static std::chrono::steady_clock::time_point g_t0;

static void output_loop(Encoder *enc, const Options *o) {
    std::vector<uint8_t> hdr(12);
    while (!g_stop) {
        MediaBufferWrapper *b = nullptr;
        if (!media_codec_source_read(enc->codec, &b) || !b) { LOGE("encoder read failed"); break; }
        auto *data = (uint8_t *)media_buffer_get_data(b) + media_buffer_get_range_offset(b);
        size_t len = media_buffer_get_range_length(b);
        auto *meta = media_buffer_get_meta_data(b);
        int32_t is_cfg = 0; int64_t ts = 0;
        media_meta_data_find_int32(meta, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_IS_CODEC_CONFIG), &is_cfg);
        media_meta_data_find_int64(meta, media_meta_data_get_key_id(MEDIA_META_DATA_KEY_TIME), &ts);
        media_meta_data_release(meta);
        int64_t t_out = now_us(g_t0);
        bool ok = true;
        bool idr = !is_cfg && nal_has_idr(data, len);
        if (o->send_frame_meta) {
            uint64_t pf = is_cfg ? (1ULL << 62) : ((uint64_t)ts & ((1ULL << 61) - 1));
            if (idr) pf |= 1ULL << 61;
            put_be64(hdr.data(), pf); put_be32(hdr.data() + 8, (uint32_t)len);
            ok = write_all(g_video_fd, hdr.data(), 12);
        }
        ok = ok && write_all(g_video_fd, data, len);
        media_buffer_release(b);
        if (!is_cfg) {
            std::lock_guard<std::mutex> l(g_stats.m);
            g_stats.frames_out++; g_stats.bytes += (long)len; if (idr) g_stats.keyframes++; if (len > 1500) g_stats.changed++;   // tiny P-frames = nothing moved
            g_stats.enc_ms += (t_out - ts) / 1000.0; g_stats.send_ms += (now_us(g_t0) - t_out) / 1000.0;
        }
        if (!ok) { LOGI("video socket closed"); break; }
    }
    g_stop = true;
}

// ---------------------------------------------------------------- capture
static int align2(int v) { return v & ~1; }
static constexpr int kMirBuffers = 6;      // screencast buffer ring
static constexpr int kMaxInFlight = 2;     // frames the encoder may hold (must stay <= kMirBuffers - 2)

// The scrcpy HID mouse is a normal relative mouse for Mir/Lomiri. Lomiri's settings only expose an
// acceleration *bias*, but Mir's input config also has a flat profile: switch the scrcpy pointer
// devices to it (constant factor 1.0 = raw PC mouse counts) and apply the wheel multiplier there.
static MirConnection *g_map_conn;
static void configure_mouse_devices() {
    for (int attempt = 0; attempt < 40 && !g_stop; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MirConnection *conn = g_map_conn;
        if (!conn) continue;
        MirInputConfig *cfg = mir_connection_create_input_config(conn);
        if (!cfg) continue;
        int changed = 0, found = 0;
        for (size_t i = 0; i < mir_input_config_device_count(cfg); i++) {
            MirInputDevice *d = mir_input_config_get_mutable_device(cfg, i);
            const char *nm = mir_input_device_get_name(d);
            if (!nm || strncmp(nm, "scrcpy mouse", 12)) continue;
            MirPointerConfig *pc = mir_input_device_get_mutable_pointer_config(d);
            if (!pc) continue;
            found++;
            double m = g_cfg.scroll_multiplier;
            if (mir_pointer_config_get_acceleration(pc) != mir_pointer_acceleration_none ||
                mir_pointer_config_get_acceleration_bias(pc) != 0.0 ||
                mir_pointer_config_get_vertical_scroll_scale(pc) != m) {
                mir_pointer_config_set_acceleration(pc, mir_pointer_acceleration_none);
                mir_pointer_config_set_acceleration_bias(pc, 0.0);
                mir_pointer_config_set_vertical_scroll_scale(pc, m);
                mir_pointer_config_set_horizontal_scroll_scale(pc, m);
                changed++;
            }
        }
        if (changed) mir_connection_set_base_input_config(conn, cfg);
        mir_input_config_release(cfg);
        if (found && !changed) {
            LOGI("mouse: flat acceleration (1:1), wheel x%.1f applied", g_cfg.scroll_multiplier);
            return;
        }
        if (changed) LOGI("mouse: requested flat acceleration for %d device(s)", changed);
    }
    LOGE("mouse: could not apply flat acceleration (device not seen by Mir or request ignored)");
}

// pointer mapping: bounding box of the powered-on outputs (what Mir maps absolute devices onto)
static MirRectangle g_map_region;
static void update_pointer_map() {
    MirDisplayConfiguration *c = mir_connection_create_display_config(g_map_conn);
    if (!c) return;
    bool any = false; int minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (unsigned i = 0; i < c->num_outputs; i++) {
        auto &op = c->outputs[i];
        if (!op.connected || !op.used || op.current_mode >= op.num_modes) continue;
        int ow = (int)op.modes[op.current_mode].horizontal_resolution, oh = (int)op.modes[op.current_mode].vertical_resolution;
        if (op.orientation == mir_orientation_left || op.orientation == mir_orientation_right) std::swap(ow, oh);
        bool on = op.power_mode == mir_power_mode_on;
        LOGI("output %u: %dx%d at %d,%d power %s", op.output_id, ow, oh, op.position_x, op.position_y, on ? "on" : "off");
        if (!on) continue;
        if (!any) { minx = op.position_x; miny = op.position_y; maxx = minx + ow; maxy = miny + oh; any = true; }
        minx = std::min(minx, op.position_x); miny = std::min(miny, op.position_y);
        maxx = std::max(maxx, op.position_x + ow); maxy = std::max(maxy, op.position_y + oh);
    }
    mir_display_config_destroy(c);
    const MirRectangle &r = g_map_region;
    if (!any) { minx = r.left; miny = r.top; maxx = r.left + (int)r.width; maxy = r.top + (int)r.height; }
    std::lock_guard<std::mutex> l(g_pmap_m);
    PointerMap n = g_pmap;
    n.rx = r.left - minx; n.ry = r.top - miny; n.rw = (int)r.width; n.rh = (int)r.height;
    n.bw = maxx - minx; n.bh = maxy - miny;
    if (n.rx != g_pmap.rx || n.ry != g_pmap.ry || n.rw != g_pmap.rw || n.rh != g_pmap.rh || n.bw != g_pmap.bw || n.bh != g_pmap.bh) {
        n.gen = g_pmap.gen + 1; g_pmap = n;
        LOGI("pointer map %d: region %d,%d %dx%d in %dx%d", n.gen, n.rx, n.ry, n.rw, n.rh, n.bw, n.bh);
    }
}

// Phone panel refresh while the desktop is streamed: Lomiri composites at the panel rate (120 Hz),
// and sampling that at 60 fps gives uneven motion. Drop the panel to 60 Hz for the session and put
// it back afterwards (also from the exit watchdog).
static std::atomic<double> g_saved_refresh{0};
static std::mutex g_refresh_m;
static bool set_phone_refresh(MirConnection *conn, double target, double *previous) {
    MirDisplayConfig *cfg = mir_connection_create_display_configuration(conn);
    if (!cfg) return false;
    bool done = false;
    for (int i = 0; i < mir_display_config_get_num_outputs(cfg); i++) {
        MirOutput *out = mir_display_config_get_mutable_output(cfg, i);
        const MirOutputMode *cur = mir_output_get_current_mode(out);
        if (!cur || mir_output_get_position_x(out) != 0 || mir_output_get_position_y(out) != 0) continue;   // the phone panel sits at 0,0
        int w = mir_output_mode_get_width(cur), h = mir_output_mode_get_height(cur);
        double now = mir_output_mode_get_refresh_rate(cur);
        if (previous) *previous = now;
        const MirOutputMode *best = nullptr;
        for (int m = 0; m < mir_output_get_num_modes(out); m++) {
            const MirOutputMode *md = mir_output_get_mode(out, (size_t)m);
            if (previous) LOGI("panel mode %d: %dx%d @ %.1f Hz%s", m, mir_output_mode_get_width(md), mir_output_mode_get_height(md),
                               mir_output_mode_get_refresh_rate(md), md == cur ? " (current)" : "");
            if (mir_output_mode_get_width(md) != w || mir_output_mode_get_height(md) != h) continue;
            if (!best || fabs(mir_output_mode_get_refresh_rate(md) - target) < fabs(mir_output_mode_get_refresh_rate(best) - target)) best = md;
        }
        if (!best) break;
        if (fabs(mir_output_mode_get_refresh_rate(best) - now) < 1) { LOGI("panel already at %.1f Hz", now); done = true; break; }
        mir_output_set_current_mode(out, best);
        mir_connection_preview_base_display_configuration(conn, cfg, 15);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        mir_connection_confirm_base_display_configuration(conn, cfg);
        LOGI("panel refresh %.1f -> %.1f Hz requested", now, mir_output_mode_get_refresh_rate(best));
        done = true;
        break;
    }
    mir_display_config_release(cfg);
    return done;
}
static void restore_phone_refresh() {
    std::lock_guard<std::mutex> l(g_refresh_m);
    double r = g_saved_refresh.exchange(0);
    if (r > 0 && g_map_conn) { set_phone_refresh(g_map_conn, r, nullptr); LOGI("panel refresh restored to %.0f Hz", r); }
}

static int run_video(const Options &o) {
    MirConnection *conn = mir_connect_sync("/run/mir_socket", "larry-scrcpy");
    if (!mir_connection_is_valid(conn)) { LOGE("Mir connect: %s", mir_connection_get_error_message(conn)); return 1; }
    MirDisplayConfiguration *cfg = mir_connection_create_display_config(conn);
    MirDisplayOutput *out = nullptr;
    for (unsigned i = 0; cfg && i < cfg->num_outputs; i++) {
        auto &op = cfg->outputs[i];
        if (op.connected && op.used && op.current_mode < op.num_modes) { out = &op; break; }
    }
    if (!out) { LOGE("no active display output"); return 1; }
    const MirDisplayMode &mode = out->modes[out->current_mode];
    int scr_w = (int)mode.horizontal_resolution, scr_h = (int)mode.vertical_resolution;
    LOGI("phone display %dx%d @ %.0f Hz", scr_w, scr_h, mode.refresh_rate);

    MirRectangle region;
    int w, h;
    if (o.new_display) {   // region outside the phone screen -> Mir adds a virtual output (second monitor)
        w = o.nd_width; h = o.nd_height;
        region.left = scr_w; region.top = 0; region.width = (unsigned)w; region.height = (unsigned)h;
    } else {               // mirror the phone screen, scaled down to max_size
        region.left = 0; region.top = 0; region.width = (unsigned)scr_w; region.height = (unsigned)scr_h;
        w = scr_w; h = scr_h;
        int m = o.max_size;
        if (m > 0 && (w > m || h > m)) {
            if (w >= h) { h = h * m / w; w = m; } else { w = w * m / h; h = m; }
        }
    }
    w = align2(w); h = align2(h);
    mir_display_config_destroy(cfg);

    MirScreencastSpec *spec = mir_create_screencast_spec(conn);
    mir_screencast_spec_set_width(spec, (unsigned)w);
    mir_screencast_spec_set_height(spec, (unsigned)h);
    mir_screencast_spec_set_capture_region(spec, &region);
    unsigned nfmt = 0; MirPixelFormat fmt;
    mir_connection_get_available_surface_formats(conn, &fmt, 1, &nfmt);
    if (!nfmt) { LOGE("no pixel format"); return 1; }
    mir_screencast_spec_set_pixel_format(spec, fmt);
    mir_screencast_spec_set_mirror_mode(spec, mir_mirror_mode_vertical);
    mir_screencast_spec_set_number_of_buffers(spec, kMirBuffers);
    MirScreencast *sc = mir_screencast_create_sync(spec);
    mir_screencast_spec_release(spec);
    if (!mir_screencast_is_valid(sc)) { LOGE("screencast: %s", mir_screencast_get_error_message(sc)); return 1; }
    MirBufferStream *bs = mir_screencast_get_buffer_stream(sc);
    g_map_region = region; g_map_conn = conn;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    update_pointer_map();
    if (o.new_display && g_cfg.desktop_refresh > 0) {
        double prev = 0;
        if (set_phone_refresh(conn, g_cfg.desktop_refresh, &prev) && fabs(prev - g_cfg.desktop_refresh) >= 1) g_saved_refresh = prev;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        if (auto *c = mir_connection_create_display_configuration(conn)) {
            for (int i = 0; i < mir_display_config_get_num_outputs(c); i++) {
                const MirOutput *op = mir_display_config_get_output(c, i);
                if (const MirOutputMode *m = mir_output_get_current_mode(op))
                    LOGI("output %d now %dx%d @ %.1f Hz", mir_output_get_id(op), mir_output_mode_get_width(m), mir_output_mode_get_height(m), mir_output_mode_get_refresh_rate(m));
            }
            mir_display_config_release(c);
        }
    }
    mir_connection_set_display_config_change_callback(conn, [](MirConnection *, void *) { update_pointer_map(); }, nullptr);
    LOGI("capturing %s region (%d,%d %ux%u) as %dx%d", o.new_display ? "virtual-display" : "mirror",
         region.left, region.top, region.width, region.height, w, h);

    if (o.send_stream_meta) {
        uint8_t b[16];
        put_be32(b, g_hevc ? 0x68323635 : 0x68323634);          // "h265" / "h264"
        put_be32(b + 4, 0x80000000u); put_be32(b + 8, (uint32_t)w); put_be32(b + 12, (uint32_t)h);  // session packet
        if (!write_all(g_video_fd, b, 16)) return 1;
    }

    Encoder enc;
    enc.options = o.codec_options;
    g_t0 = std::chrono::steady_clock::now();
    if (!enc.start(w, h, o.max_fps, o.bit_rate)) {
        LOGE("encoder refused profile/level/operating-rate, retrying with defaults");
        enc.use_operating_rate = false; enc.use_profile = false;
        if (!enc.start(w, h, o.max_fps, o.bit_rate)) return 1;
    }
    std::thread outt(output_loop, &enc, &o);
    std::thread statt([] {
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::lock_guard<std::mutex> l(g_stats.m);
            double fi = g_stats.frames_in, fo = g_stats.frames_out ? g_stats.frames_out : 1;
            long gpu = 0; if (FILE *g = fopen("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "r")) { if (fscanf(g, "%ld", &gpu) != 1) gpu = 0; fclose(g); }
            LOGI("stats/2s: keyboard reports %ld, mouse reports %ld | content changes %.1f/s, compositor captures %.1f/s | captured %.1f fps, encoded %.1f fps, %.2f Mbit/s, avg frame %.1f KB, keyframes %ld | "
                 "wait-encoder %.1f ms, mir-swap %.1f ms, capture->encoded %.1f ms, socket %.2f ms | gpu %ld MHz",
                 g_stats.keys, g_stats.mouse, g_stats.changed / 2.0, g_stats.captures / 2.0, fi / 2, g_stats.frames_out / 2.0, g_stats.bytes * 8 / 2e6, g_stats.bytes / fo / 1024, g_stats.keyframes,
                 fi ? g_stats.wait_ms / fi : 0, fi ? g_stats.swap_ms / fi : 0, g_stats.enc_ms / fo, g_stats.send_ms / fo, gpu / 1000000);
            g_stats.frames_in = g_stats.frames_out = g_stats.bytes = g_stats.keyframes = g_stats.changed = g_stats.captures = g_stats.keys = g_stats.mouse = 0;
            g_stats.swap_ms = g_stats.enc_ms = g_stats.send_ms = g_stats.wait_ms = 0;
        }
    });
    statt.detach();

    auto t0 = g_t0;
    // The compositor only finishes the virtual screen's frames when we take them, so pulling at a
    // higher rate (capture_fps) keeps Lomiri's second-screen rendering from stalling; only every
    // n-th capture is encoded.
    int cap_fps = g_cfg.capture_fps > o.max_fps ? g_cfg.capture_fps : o.max_fps;
    int every = (cap_fps + o.max_fps / 2) / o.max_fps; if (every < 1) every = 1;
    LOGI("capture %d fps, encode every %d -> %d fps", cap_fps, every, cap_fps / every);
    auto frame = std::chrono::microseconds(1000000 / cap_fps);
    auto next = t0;
    long n = 0;
    while (!g_stop) {
        bool encode = (n++ % every) == 0;
        int64_t ta = now_us(t0);
        if (encode) enc.wait_slots(g_cfg.max_in_flight - 1);  // keep the encoder fed, but never reuse a held buffer
        int64_t tb = now_us(t0);
        mir_buffer_stream_swap_buffers_sync(bs);                // Mir renders the region into a buffer
        int64_t ts = now_us(t0);
        { std::lock_guard<std::mutex> l(g_stats.m); g_stats.captures++; }
        if (encode) {
            MirNativeBuffer *nb = nullptr;
            mir_buffer_stream_get_current_buffer(bs, &nb);
            if (nb) {
                enc.submit(nb, ts);
                std::lock_guard<std::mutex> l(g_stats.m);
                g_stats.frames_in++; g_stats.wait_ms += (tb - ta) / 1000.0; g_stats.swap_ms += (ts - tb) / 1000.0;
            }
        }
        next += frame;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next); else next = now;
    }
    { std::lock_guard<std::mutex> l(enc.m); enc.running = false; enc.cv.notify_all(); }
    media_codec_source_stop(enc.codec);
    if (outt.joinable()) outt.join();
    restore_phone_refresh();
    mir_screencast_release_sync(sc);
    mir_connection_release(conn);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_logfile = fopen("/tmp/larry-scrcpy.log", "w");
    load_config();
    Options o = parse_options(argc, argv);
    LOGI("config: scroll_multiplier=%.2f max_in_flight=%d operating_rate=%.0f", g_cfg.scroll_multiplier, g_cfg.max_in_flight, g_cfg.operating_rate);
    LOGI("starting: video=%d audio=%d control=%d forward=%d bitrate=%d fps=%d max_size=%d new_display=%s",
         o.video, o.audio, o.control, o.tunnel_forward, o.bit_rate, o.max_fps, o.max_size,
         o.new_display ? (std::to_string(o.nd_width) + "x" + std::to_string(o.nd_height)).c_str() : "no");
    g_hevc = o.video_codec == "h265";
    if (o.video && o.video_codec != "h264" && !g_hevc) LOGE("only h264/h265 are supported (got %s); using h264", o.video_codec.c_str());
    if (!open_connection(o)) return 1;
    if (g_audio_fd >= 0) { uint8_t z[4] = {0, 0, 0, 0}; write_all(g_audio_fd, z, 4); }   // audio disabled on device
    std::thread ctl;
    if (g_control_fd >= 0) ctl = std::thread(control_loop);
    std::thread([] {   // shutdown watchdog: never linger after the PC side goes away
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        restore_phone_refresh();
        LOGI("exiting (watchdog)");
        if (g_logfile) fflush(g_logfile);
        _exit(0);
    }).detach();
    int rc = 0;
    if (o.video) rc = run_video(o);
    else while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g_stop = true;
    if (g_control_fd >= 0) shutdown(g_control_fd, SHUT_RDWR);
    if (ctl.joinable()) ctl.join();
    for (auto &d : g_uhid_devs) close(d.second);
    if (g_uinput >= 0) { ioctl(g_uinput, UI_DEV_DESTROY); close(g_uinput); }
    if (g_uinput_rel >= 0) { ioctl(g_uinput_rel, UI_DEV_DESTROY); close(g_uinput_rel); }
    LOGI("exiting (%d)", rc);
    return rc;
}
