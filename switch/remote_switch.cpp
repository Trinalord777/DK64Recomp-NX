// Developer test harness, compiled only into DK64NX_REMOTE builds (switch/build.sh with DK64NX_DEV=1): a TCP
// command server on port 28280 so a computer on the LAN can drive test sessions. It injects pad input,
// captures the presented frame, reads RDRAM and per-thread CPU load, and returns the startup experiments.
// There is no authentication, so it must never be part of a release build. Client: switch/dk64nx_remote.py
#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <switch.h>

#include "nx_pad.h"

std::string dk64nx_run_experiments();
extern "C" int dk64nx_gliden64_set(const char* name, int value);
extern "C" int dk64nx_interp_stats(char* out, int size);
extern "C" bool dk64nx_interp_capture(uint32_t* width, uint32_t* height, const uint8_t** real, const uint8_t** in_between);

namespace {
    constexpr int PORT = 28280;
    constexpr uint32_t RDRAM_LIMIT = 64u * 1024 * 1024;
    constexpr uint32_t CURRENT_MAP = 0x8076A0A8;

    // Switch buttons: the harness acts as a second physical pad, so the NX Options menu sees it too.
    struct ButtonName { const char* name; uint32_t bit; };
    constexpr ButtonName BUTTONS[] = {
        {"A", NX_A}, {"B", NX_B}, {"X", NX_X}, {"Y", NX_Y}, {"L", NX_L}, {"R", NX_R}, {"ZL", NX_ZL},
        {"ZR", NX_ZR}, {"PLUS", NX_PLUS}, {"MINUS", NX_MINUS}, {"UP", NX_UP}, {"DOWN", NX_DOWN},
        {"LEFT", NX_LEFT}, {"RIGHT", NX_RIGHT}, {"LS", NX_LSTICK}, {"RS", NX_RSTICK},
    };

    struct Stick {
        std::atomic<float> x{0.0f};
        std::atomic<float> y{0.0f};
        std::atomic<uint64_t> until{0};
    };

    std::atomic<uint32_t> remote_buttons{0};
    std::atomic<uint64_t> buttons_until{0};
    Stick left_stick;
    Stick right_stick;
    std::atomic<uint32_t> presents{0};
    std::atomic<uint8_t*> rdram{nullptr};
    std::string experiments;

    std::mutex shot_mutex;
    std::condition_variable shot_cv;
    bool shot_wanted = false;
    bool shot_ready = false;
    std::vector<uint8_t> shot_pixels;
    int shot_width = 0;
    int shot_height = 0;

    // Graphics thread timings (render_gliden64.cpp): 0 = ProcessDList, 1 = UpdateScreen (present).
    struct Timing {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> total_us{0};
        std::atomic<uint64_t> max_us{0};
    };
    Timing timings[2];

    std::string timing_report() {
        std::string out = "OK";
        const char* names[] = {"dl", "present"};
        for (int i = 0; i < 2; i++) {
            const uint64_t count = timings[i].count.exchange(0);
            const uint64_t total = timings[i].total_us.exchange(0);
            const uint64_t max = timings[i].max_us.exchange(0);
            char entry[96];
            std::snprintf(entry, sizeof(entry), " %s n=%llu avg=%lluus max=%lluus", names[i],
                          static_cast<unsigned long long>(count),
                          static_cast<unsigned long long>(count ? total / count : 0),
                          static_cast<unsigned long long>(max));
            out += entry;
        }
        return out + "\n";
    }

    // Time between presents, in buckets around 16.7 ms (one 60 Hz refresh) and 33.3 ms (two): the frame pacing.
    constexpr uint64_t PACE_LIMITS_US[] = {12000, 15500, 18000, 25000, 31000, 36000};
    constexpr const char* PACE_NAMES[] = {"<12", "12-15.5", "15.5-18", "18-25", "25-31", "31-36", ">36"};
    std::atomic<uint64_t> pace_counts[7];
    uint64_t last_present_tick = 0;  // graphics thread only

    void count_present_interval() {
        const uint64_t now = armGetSystemTick();
        if (last_present_tick != 0) {
            const uint64_t us = armTicksToNs(now - last_present_tick) / 1000;
            size_t bucket = 0;
            while (bucket < 6 && us >= PACE_LIMITS_US[bucket]) bucket++;
            pace_counts[bucket]++;
        }
        last_present_tick = now;
    }

    std::string pace_report() {
        std::string out = "OK";
        for (size_t i = 0; i < 7; i++) {
            out += " " + std::string(PACE_NAMES[i]) + "ms=" + std::to_string(pace_counts[i].exchange(0));
        }
        return out + "\n";
    }

    // The end of this run's log, which FTP cannot read while the game keeps it open.
    std::string log_tail() {
        FILE* file = std::fopen("sdmc:/switch/dk64recomp/dk64recomp.log", "r");
        if (file == nullptr) return "ERR no log\n";
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        const long start = size > 4000 ? size - 4000 : 0;
        std::fseek(file, start, SEEK_SET);
        std::string text(static_cast<size_t>(size - start), '\0');
        text.resize(std::fread(text.data(), 1, text.size(), file));
        std::fclose(file);
        return "OK\n" + text + "\nEND\n";
    }

    struct TrackedThread { std::string name; Handle handle; uint64_t last_ticks; uint64_t last_time; };
    std::mutex threads_mutex;
    std::vector<TrackedThread> threads;

    uint64_t now_ms() { return SDL_GetTicks64(); }

    bool parse_buttons(const std::string& list, uint32_t* out) {
        *out = 0;
        if (list == "none") return true;
        std::stringstream parts(list);
        std::string name;
        while (std::getline(parts, name, '+')) {
            bool found = false;
            for (const ButtonName& button : BUTTONS) {
                if (name == button.name) { *out |= button.bit; found = true; }
            }
            if (!found) return false;
        }
        return true;
    }

    std::string peek(uint32_t address, uint32_t words) {
        uint8_t* base = rdram.load();
        const uint32_t offset = (address & 0x1FFFFFFF) & ~3u;
        if (base == nullptr || words == 0 || words > 256 || offset + words * 4 > RDRAM_LIMIT) {
            return "ERR bad address or length\n";
        }
        std::string out = "OK";
        char word[12];
        for (uint32_t i = 0; i < words; i++) {
            // RDRAM keeps 32-bit words in host order, so a word read is the N64 value.
            std::snprintf(word, sizeof(word), " %08X", *reinterpret_cast<uint32_t*>(base + offset + i * 4));
            out += word;
        }
        return out + "\n";
    }

    std::string cpu_load() {
        std::lock_guard lock{threads_mutex};
        std::string out = "OK";
        for (TrackedThread& thread : threads) {
            uint64_t ticks = 0;
            if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, thread.handle, UINT64_MAX))) continue;
            const uint64_t time = armGetSystemTick();
            const double busy = thread.last_time == 0 ? 0.0 :
                100.0 * double(ticks - thread.last_ticks) / double(time - thread.last_time);
            thread.last_ticks = ticks;
            thread.last_time = time;
            char entry[96];
            std::snprintf(entry, sizeof(entry), " %s=%.0f%%", thread.name.c_str(), busy);
            out += entry;
        }
        return out + "\n";
    }

    bool send_all(int fd, const void* data, size_t size) {
        const char* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const ssize_t sent = send(fd, bytes, size, 0);
            if (sent <= 0) return false;
            bytes += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    // Waits for the graphics thread to read back the next presented frame (bottom-up RGBA).
    bool send_screenshot(int fd) {
        std::unique_lock lock{shot_mutex};
        shot_ready = false;
        shot_wanted = true;
        if (!shot_cv.wait_for(lock, std::chrono::seconds(2), [] { return shot_ready; })) {
            shot_wanted = false;
            return send_all(fd, "ERR no frame presented within 2 s\n", 34);
        }
        char header[64];
        const int length = std::snprintf(header, sizeof(header), "OK %d %d %zu\n", shot_width, shot_height,
                                         shot_pixels.size());
        return send_all(fd, header, length) && send_all(fd, shot_pixels.data(), shot_pixels.size());
    }

    std::string handle(const std::string& line, int fd, bool* binary_sent) {
        std::stringstream in(line);
        std::string command;
        in >> command;
        if (command == "press") {
            std::string list; uint64_t ms = 0;
            uint32_t bits = 0;
            if (!(in >> list >> ms) || !parse_buttons(list, &bits)) return "ERR usage: press LS+RS 200\n";
            remote_buttons = bits;
            buttons_until = now_ms() + ms;
            return "OK\n";
        }
        if (command == "stick" || command == "rstick") {
            float x = 0, y = 0; uint64_t ms = 0;
            if (!(in >> x >> y >> ms)) return "ERR usage: stick|rstick 0.0 1.0 500\n";
            Stick& stick = command == "stick" ? left_stick : right_stick;
            stick.x = x; stick.y = y;
            stick.until = now_ms() + ms;
            return "OK\n";
        }
        if (command == "ishot") {
            // Real and in-between copies of the next interpolated frame (60 fps mode), back to back.
            *binary_sent = true;
            uint32_t width = 0, height = 0;
            const uint8_t* real = nullptr;
            const uint8_t* in_between = nullptr;
            if (!dk64nx_interp_capture(&width, &height, &real, &in_between)) {
                send_all(fd, "ERR no interpolated frame within 2 s\n", 37);
                return "";
            }
            const size_t size = size_t(width) * height * 4;
            char header[64];
            const int length = std::snprintf(header, sizeof(header), "OK %u %u %zu\n", width, height, size);
            send_all(fd, header, length) && send_all(fd, real, size) && send_all(fd, in_between, size);
            return "";
        }
        if (command == "shot") {
            *binary_sent = true;
            send_screenshot(fd);
            return "";
        }
        if (command == "peek") {
            std::string address; uint32_t words = 1;
            if (!(in >> address)) return "ERR usage: peek 8076A0A8 4\n";
            in >> words;
            return peek(static_cast<uint32_t>(std::stoul(address, nullptr, 16)), words);
        }
        if (command == "setbits") {
            // ORs a mask into one RDRAM byte (e.g. permanent flags), leaving its other bits alone.
            std::string address; unsigned mask = 0;
            if (!(in >> address >> std::hex >> mask) || mask > 0xFF) return "ERR usage: setbits 807ED054 40\n";
            uint8_t* base = rdram.load();
            const uint32_t offset = static_cast<uint32_t>(std::stoul(address, nullptr, 16)) & 0x1FFFFFFF;
            if (base == nullptr || offset >= RDRAM_LIMIT) return "ERR bad address\n";
            base[offset ^ 3] |= static_cast<uint8_t>(mask);
            char out[32];
            std::snprintf(out, sizeof(out), "OK now=%02X\n", base[offset ^ 3]);
            return out;
        }
        if (command == "status") {
            uint8_t* base = rdram.load();
            const uint32_t map = base ? *reinterpret_cast<uint32_t*>(base + (CURRENT_MAP & 0x1FFFFFFF)) : 0;
            char out[96];
            std::snprintf(out, sizeof(out), "OK map=%u presents=%u uptime_ms=%llu\n", map, presents.load(),
                          static_cast<unsigned long long>(now_ms()));
            return out;
        }
        if (command == "cpu") return cpu_load();
        if (command == "timing") return timing_report();
        if (command == "mtx") {
            char out[256];
            dk64nx_interp_stats(out, sizeof(out));
            return std::string("OK ") + out + "\n";
        }
        if (command == "gl") {
            std::string name; int value = 0;
            if (!(in >> name >> value)) return "ERR usage: gl copy 0\n";
            const int previous = dk64nx_gliden64_set(name.c_str(), value);
            return previous < 0 ? "ERR unknown setting\n" : "OK previous=" + std::to_string(previous) + "\n";
        }
        if (command == "exp") return "OK\n" + experiments + "END\n";
        if (command == "pace") return pace_report();
        if (command == "log") return log_tail();
        return "ERR commands: press stick rstick shot peek setbits status cpu timing gl exp\n";
    }

    // main stops the server (dk64nx_remote_stop) before socketExit: a thread still inside a socket call afterwards
    // dereferences the freed socket table (_socketGetFd) and crashed every Quit/Restart Game. So the server only
    // blocks POLL_MS at a time.
    constexpr int POLL_MS = 200;
    std::atomic<bool> stopping{false};
    std::thread server_thread;

    // Waits until fd is readable; false when the server is stopping.
    bool wait_readable(int fd) {
        while (!stopping) {
            pollfd entry{fd, POLLIN, 0};
            if (poll(&entry, 1, POLL_MS) > 0) return true;
        }
        return false;
    }

    void serve_client(int fd) {
        std::string pending;
        char buffer[512];
        while (wait_readable(fd)) {
            const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            pending.append(buffer, static_cast<size_t>(got));
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                bool binary_sent = false;
                const std::string reply = handle(line, fd, &binary_sent);
                if (!binary_sent && !send_all(fd, reply.data(), reply.size())) return;
            }
        }
    }

    void server_loop() {
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 1) != 0) {
            std::printf("[DK64NX] remote: cannot listen on port %d\n", PORT);
            return;
        }
        std::printf("[DK64NX] remote: listening on port %d\n", PORT);
        while (wait_readable(listener)) {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) continue;
            serve_client(client);
            close(client);
        }
        close(listener);
    }

}

void dk64nx_remote_stop() {
    stopping = true;
    if (server_thread.joinable()) server_thread.join();
}

void dk64nx_remote_start() {
    experiments = dk64nx_run_experiments();
    std::fputs(experiments.c_str(), stdout);
    server_thread = std::thread(server_loop);
}

void dk64nx_remote_time(int kind, uint64_t start_tick) {
    const uint64_t us = armTicksToNs(armGetSystemTick() - start_tick) / 1000;
    Timing& timing = timings[kind];
    timing.count++;
    timing.total_us += us;
    uint64_t max = timing.max_us.load();
    while (us > max && !timing.max_us.compare_exchange_weak(max, us)) {}
}

void dk64nx_remote_set_rdram(uint8_t* base) {
    rdram = base;
}

extern "C" void dk64nx_remote_track_thread(const char* name, Handle handle) {
    std::lock_guard lock{threads_mutex};
    threads.push_back({name, handle, 0, 0});
}

// Adds the remote buttons to the pad and overrides its sticks while a remote command is active.
bool dk64nx_remote_apply(NxPad* pad) {
    const uint64_t now = now_ms();
    bool active = false;
    if (now < buttons_until.load()) {
        pad->buttons |= remote_buttons.load();
        active = true;
    }
    if (now < left_stick.until.load()) {
        pad->lx = left_stick.x.load();
        pad->ly = left_stick.y.load();
        active = true;
    }
    if (now < right_stick.until.load()) {
        pad->rx = right_stick.x.load();
        pad->ry = right_stick.y.load();
        active = true;
    }
    return active;
}

// Graphics thread, right before the swap: the back buffer holds the frame about to be shown.
extern "C" void dk64nx_remote_on_swap(int width, int height) {
    presents++;
    count_present_interval();
    std::lock_guard lock{shot_mutex};
    if (!shot_wanted) return;
    using GetIntegerv = void (*)(unsigned, int*);
    using BindFramebuffer = void (*)(unsigned, unsigned);
    using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
    static auto get_integerv = reinterpret_cast<GetIntegerv>(SDL_GL_GetProcAddress("glGetIntegerv"));
    static auto bind_framebuffer = reinterpret_cast<BindFramebuffer>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    static auto read_pixels = reinterpret_cast<ReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));
    constexpr unsigned READ_FRAMEBUFFER = 0x8CA8, READ_FRAMEBUFFER_BINDING = 0x8CAA, RGBA = 0x1908,
                       UNSIGNED_BYTE = 0x1401;
    int previous = 0;
    get_integerv(READ_FRAMEBUFFER_BINDING, &previous);
    bind_framebuffer(READ_FRAMEBUFFER, 0);
    shot_pixels.resize(static_cast<size_t>(width) * height * 4);
    read_pixels(0, 0, width, height, RGBA, UNSIGNED_BYTE, shot_pixels.data());
    // GLideN64 caches its framebuffer bindings: leave the binding exactly as found.
    bind_framebuffer(READ_FRAMEBUFFER, static_cast<unsigned>(previous));
    shot_width = width;
    shot_height = height;
    shot_wanted = false;
    shot_ready = true;
    shot_cv.notify_all();
}
