// Host check of nx_cheats.cpp: c++ -std=c++20 -DNX_CHEATS_DIR='"/tmp/dir"' nx_cheats.cpp tests/nx_cheats_test.cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../nx_cheats.h"

namespace {
    // N64 byte address -> host RDRAM index (words kept in host order).
    uint8_t byte_at(const std::vector<uint8_t>& rdram, uint32_t address) { return rdram[(address & 0xFFFFFF) ^ 3]; }
    uint16_t half_at(const std::vector<uint8_t>& rdram, uint32_t address) {
        uint16_t value;
        std::memcpy(&value, &rdram[(address & 0xFFFFFF) ^ 2], 2);
        return value;
    }
}

int main() {
    std::remove(NX_CHEATS_DIR "/cheats_enabled.txt");
    FILE* file = std::fopen(NX_CHEATS_DIR "/cheats.cht", "w");
    std::fputs("cheats = 5\n\n"
               "cheat0_desc = \"Donkey Kong\\Infinite Ammo\"\n"
               "cheat0_code = \"807FCC45 0096\"\n"
               "cheat0_enable = true\n\n"
               "cheat1_desc = \"Infinite\\Health\"\n"
               "cheat1_code = \"817FCC4C 041F\"\n"
               "cheat1_enable = false\n\n"
               "cheat2_desc = \"Conditional\"\n"
               "cheat2_code = \"D07FCC45 0096;817FCC50 1234+D07FCC45 0001;817FCC52 BEEF\"\n\n"
               "cheat3_desc = \"Boot code\"\n"
               "cheat3_code = \"F1000400 0000\"\n\n"
               "cheat4_desc = \"Empty\"\n", file);
    std::fclose(file);

    nx::cheats::load();
    assert(nx::cheats::count() == 4);  // "Empty" has no code
    assert(std::strcmp(nx::cheats::name(0), "Donkey Kong / Infinite Ammo") == 0);
    assert(nx::cheats::enabled(0) && !nx::cheats::enabled(1));
    assert(!nx::cheats::supported(3));

    std::vector<uint8_t> rdram(8 * 1024 * 1024);
    nx::cheats::set_rdram(rdram.data());
    nx::cheats::toggle(1);
    nx::cheats::toggle(2);
    nx::cheats::toggle(3);  // unsupported: stays off
    assert(!nx::cheats::enabled(3));
    nx::cheats::apply();
    assert(byte_at(rdram, 0x807FCC45) == 0x96);
    assert(half_at(rdram, 0x817FCC4C) == 0x041F);
    assert(half_at(rdram, 0x817FCC50) == 0x1234);   // condition met (ammo is 0x96)
    assert(half_at(rdram, 0x817FCC52) == 0);        // condition not met

    // The menu's choices survive a restart.
    nx::cheats::save();
    nx::cheats::load();
    assert(nx::cheats::enabled(0) && nx::cheats::enabled(1) && nx::cheats::enabled(2) && !nx::cheats::enabled(3));
    std::remove(NX_CHEATS_DIR "/cheats.cht");
    std::remove(NX_CHEATS_DIR "/cheats_enabled.txt");
    std::puts("nx_cheats: OK");
    return 0;
}
