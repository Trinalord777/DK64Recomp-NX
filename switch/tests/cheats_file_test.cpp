// Host check of the shipped switch/cheats/cheats.cht, from switch/:
// c++ -std=c++20 -DNX_CHEATS_DIR='"cheats"' nx_cheats.cpp tests/cheats_file_test.cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../nx_cheats.h"

int main() {
    nx::cheats::load();
    assert(nx::cheats::count() == 64);
    for (int i = 0; i < nx::cheats::count(); i++) {
        assert(nx::cheats::supported(i));
        assert(!nx::cheats::enabled(i));
        assert(std::strstr(nx::cheats::name(i), "Free Pass") == nullptr);
    }
    // Health writes only the pending heal byte (0x807FCC4D), never the melon count at 0x807FCC4C.
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    uint8_t* rdram = memory.data();
    for (int i = 0; i < nx::cheats::count(); i++) {
        if (std::strcmp(nx::cheats::name(i), "Infinite / Health") == 0) nx::cheats::toggle(i);
    }
    nx::cheats::set_rdram(rdram);
    nx::cheats::apply();
    assert(rdram[0x7FCC4C ^ 3] == 0);
    assert(rdram[0x7FCC4D ^ 3] == 0x1F);
    std::puts("cheats_file: OK");
    return 0;
}
