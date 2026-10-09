#include "nx_cheats.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef NX_CHEATS_DIR
#define NX_CHEATS_DIR "sdmc:/switch/dk64recomp"
#endif

namespace {
    constexpr const char* CHEATS_PATH = NX_CHEATS_DIR "/cheats.cht";
    constexpr const char* ENABLED_PATH = NX_CHEATS_DIR "/cheats_enabled.txt";
    // GameShark addresses point into the console's 8 MiB.
    constexpr uint32_t RDRAM_LIMIT = 8 * 1024 * 1024;

    struct Op {
        uint8_t type;
        uint32_t offset;
        uint16_t value;
    };

    struct Cheat {
        std::string description;  // libretro's, groups separated by '\'
        std::string name;         // shown in the menu
        std::vector<Op> ops;
        bool supported = true;
        bool enabled = false;
    };

    std::vector<Cheat> cheat_list;
    std::atomic<uint8_t*> rdram{nullptr};
    bool dirty = false;

    bool known_type(uint8_t type) {
        return type == 0x80 || type == 0x81 || type == 0xA0 || type == 0xA1 || (type >= 0xD0 && type <= 0xD3);
    }

    // "8065CC75 0001;8065CC79 0001", lines separated by ';' or '+'.
    void parse_code(const std::string& code, Cheat* cheat) {
        size_t start = 0;
        while (start <= code.size()) {
            const size_t end = code.find_first_of(";+", start);
            const std::string line = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
            unsigned address = 0;
            unsigned value = 0;
            if (std::sscanf(line.c_str(), " %8x %4x", &address, &value) == 2) {
                const Op op{static_cast<uint8_t>(address >> 24), address & 0x00FFFFFF, static_cast<uint16_t>(value)};
                cheat->supported = cheat->supported && known_type(op.type) && op.offset < RDRAM_LIMIT;
                cheat->ops.push_back(op);
            }
            else if (line.find_first_not_of(" \t") != std::string::npos) {
                cheat->supported = false;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    std::string display_name(const std::string& description) {
        std::string name;
        for (char c : description) {
            if (c == '\\') name += " / ";
            else name += c;
        }
        return name;
    }

    // cheatN_desc = "...", cheatN_code = "...", cheatN_enable = true|false
    void read_cht(std::vector<bool>* default_on) {
        FILE* file = std::fopen(CHEATS_PATH, "r");
        if (file == nullptr) return;
        char line[1024];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            int index = 0;
            char field[16];
            int value_at = 0;
            if (std::sscanf(line, "cheat%d_%15[a-z] = %n", &index, field, &value_at) != 2 || index < 0 || index > 4096) {
                continue;
            }
            std::string value = line + value_at;
            value.erase(value.find_last_not_of(" \r\n") + 1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            if (cheat_list.size() <= static_cast<size_t>(index)) {
                cheat_list.resize(index + 1);
                default_on->resize(index + 1);
            }
            if (std::strcmp(field, "desc") == 0) cheat_list[index].description = value;
            else if (std::strcmp(field, "code") == 0) parse_code(value, &cheat_list[index]);
            else if (std::strcmp(field, "enable") == 0) (*default_on)[index] = value == "true";
        }
        std::fclose(file);
    }
}

void nx::cheats::load() {
    cheat_list.clear();
    std::vector<bool> default_on;
    read_cht(&default_on);
    std::vector<Cheat> kept;
    std::vector<bool> kept_on;
    for (size_t i = 0; i < cheat_list.size(); i++) {
        if (cheat_list[i].ops.empty()) continue;
        cheat_list[i].name = display_name(cheat_list[i].description);
        kept.push_back(cheat_list[i]);
        kept_on.push_back(default_on[i]);
    }
    cheat_list = kept;
    // The cheat_list left on last time, by description; the .cht's own flags until the menu has saved once.
    FILE* file = std::fopen(ENABLED_PATH, "r");
    for (size_t i = 0; i < cheat_list.size(); i++) cheat_list[i].enabled = file == nullptr && kept_on[i];
    if (file == nullptr) return;
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        line[std::strcspn(line, "\r\n")] = '\0';
        for (Cheat& cheat : cheat_list) {
            if (cheat.description == line) cheat.enabled = true;
        }
    }
    std::fclose(file);
}

void nx::cheats::set_rdram(uint8_t* base) {
    rdram = base;
}

// RDRAM keeps 32-bit words in host order: a byte lives at address ^ 3 and a halfword at address ^ 2 (MEM_B, MEM_H).
void nx::cheats::apply() {
    uint8_t* base = rdram.load();
    if (base == nullptr) return;
    for (const Cheat& cheat : cheat_list) {
        if (!cheat.enabled || !cheat.supported) continue;
        bool skip_next = false;
        for (const Op& op : cheat.ops) {
            if (skip_next) {
                skip_next = false;
                continue;
            }
            uint8_t& byte = base[op.offset ^ 3];
            uint16_t& half = *reinterpret_cast<uint16_t*>(base + ((op.offset & ~1u) ^ 2));
            switch (op.type) {
                case 0x80: case 0xA0: byte = static_cast<uint8_t>(op.value); break;
                case 0x81: case 0xA1: half = op.value; break;
                case 0xD0: skip_next = byte != static_cast<uint8_t>(op.value); break;
                case 0xD1: skip_next = half != op.value; break;
                case 0xD2: skip_next = byte == static_cast<uint8_t>(op.value); break;
                case 0xD3: skip_next = half == op.value; break;
            }
        }
    }
}

int nx::cheats::count() { return static_cast<int>(cheat_list.size()); }
const char* nx::cheats::name(int index) { return cheat_list[index].name.c_str(); }
bool nx::cheats::supported(int index) { return cheat_list[index].supported; }
bool nx::cheats::enabled(int index) { return cheat_list[index].enabled; }

void nx::cheats::toggle(int index) {
    if (!cheat_list[index].supported) return;
    cheat_list[index].enabled = !cheat_list[index].enabled;
    dirty = true;
}

void nx::cheats::save() {
    if (!dirty) return;
    FILE* file = std::fopen(ENABLED_PATH, "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[DK64NX] cannot write %s\n", ENABLED_PATH);
        return;
    }
    for (const Cheat& cheat : cheat_list) {
        if (cheat.enabled) std::fprintf(file, "%s\n", cheat.description.c_str());
    }
    std::fclose(file);
    dirty = false;
}
