// Developer test harness, compiled only into DK64NX_REMOTE builds (switch/build.sh with DK64NX_DEV=1): a TCP
// command server on port 28280 so a computer on the LAN can drive test sessions. It injects pad input,
// captures the presented frame, reads RDRAM and per-thread CPU load, and returns the startup experiments.
// There is no authentication, so it must never be part of a release build. Client: switch/dk64nx_remote.py
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <switch.h>

std::string dk64nx_run_experiments();

namespace {
    constexpr int PORT = 28280;
    constexpr uint32_t RDRAM_LIMIT = 64u * 1024 * 1024;
    constexpr uint32_t CURRENT_MAP = 0x8076A0A8;

    struct ButtonName { const char* name; uint16_t bit; };
    constexpr ButtonName BUTTONS[] = {
        {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000}, {"DU", 0x0800}, {"DD", 0x0400},
        {"DL", 0x0200}, {"DR", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CU", 0x0008}, {"CD", 0x0004},
        {"CL", 0x0002}, {"CR", 0x0001},
    };

    std::atomic<uint32_t> remote_buttons{0};
    std::atomic<uint64_t> buttons_until{0};
    std::atomic<float> stick_x{0.0f};
    std::atomic<float> stick_y{0.0f};
    std::atomic<uint64_t> stick_until{0};
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
            if (!(in >> list >> ms) || !parse_buttons(list, &bits)) return "ERR usage: press A+B 200\n";
            remote_buttons = bits;
            buttons_until = now_ms() + ms;
            return "OK\n";
        }
        if (command == "stick") {
            float x = 0, y = 0; uint64_t ms = 0;
            if (!(in >> x >> y >> ms)) return "ERR usage: stick 0.0 1.0 500\n";
            stick_x = x; stick_y = y;
            stick_until = now_ms() + ms;
            return "OK\n";
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
        if (command == "status") {
            uint8_t* base = rdram.load();
            const uint32_t map = base ? *reinterpret_cast<uint32_t*>(base + (CURRENT_MAP & 0x1FFFFFFF)) : 0;
            char out[96];
            std::snprintf(out, sizeof(out), "OK map=%u presents=%u uptime_ms=%llu\n", map, presents.load(),
                          static_cast<unsigned long long>(now_ms()));
            return out;
        }
        if (command == "cpu") return cpu_load();
        if (command == "exp") return "OK\n" + experiments + "END\n";
        return "ERR commands: press stick shot peek status cpu exp\n";
    }

    void serve_client(int fd) {
        std::string pending;
        char buffer[512];
        while (true) {
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
        while (true) {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) continue;
            serve_client(client);
            close(client);
        }
    }
}

void dk64nx_remote_start() {
    experiments = dk64nx_run_experiments();
    std::fputs(experiments.c_str(), stdout);
    std::thread(server_loop).detach();
}

void dk64nx_remote_set_rdram(uint8_t* base) {
    rdram = base;
}

extern "C" void dk64nx_remote_track_thread(const char* name, Handle handle) {
    std::lock_guard lock{threads_mutex};
    threads.push_back({name, handle, 0, 0});
}

// Adds the remote buttons to the pad state and overrides the stick while a remote command is active.
bool dk64nx_remote_apply(uint16_t* buttons, float* x, float* y) {
    const uint64_t now = now_ms();
    bool active = false;
    if (now < buttons_until.load()) {
        *buttons |= static_cast<uint16_t>(remote_buttons.load());
        active = true;
    }
    if (now < stick_until.load()) {
        *x = stick_x.load();
        *y = stick_y.load();
        active = true;
    }
    return active;
}

// Graphics thread, right before the swap: the back buffer holds the frame about to be shown.
extern "C" void dk64nx_remote_on_swap(int width, int height) {
    presents++;
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
