// The game's RAM for NX Options' Dev Tools, by N64 address (KSEG0, 0x80000000-0x807FFFFF). The recompiled game keeps
// each 32-bit word in host order: a byte lives at address ^ 3 and a halfword at address ^ 2 (librecomp's MEM_B/MEM_H).
// Set by the renderer once RDRAM exists; null before, and every read then returns 0.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace nx::ram {
    inline std::atomic<uint8_t*> base{nullptr};
    constexpr uint32_t SIZE = 8 * 1024 * 1024;

    inline bool valid(uint32_t address) { return base.load() != nullptr && address >= 0x80000000u && address < 0x80000000u + SIZE; }
    inline uint32_t offset(uint32_t address) { return address & (SIZE - 1); }

    inline uint8_t u8(uint32_t address) { return valid(address) ? base.load()[offset(address) ^ 3] : 0; }
    inline uint16_t u16(uint32_t address) {
        uint16_t value = 0;
        if (valid(address)) std::memcpy(&value, base.load() + ((offset(address) & ~1u) ^ 2), 2);
        return value;
    }
    inline uint32_t u32(uint32_t address) {
        uint32_t value = 0;
        if (valid(address)) std::memcpy(&value, base.load() + (offset(address) & ~3u), 4);
        return value;
    }
    inline float f32(uint32_t address) {
        const uint32_t bits = u32(address);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    inline void set_u8(uint32_t address, uint8_t value) { if (valid(address)) base.load()[offset(address) ^ 3] = value; }
    inline void set_u16(uint32_t address, uint16_t value) {
        if (valid(address)) std::memcpy(base.load() + ((offset(address) & ~1u) ^ 2), &value, 2);
    }
    inline void set_u32(uint32_t address, uint32_t value) {
        if (valid(address)) std::memcpy(base.load() + (offset(address) & ~3u), &value, 4);
    }
    inline void set_f32(uint32_t address, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        set_u32(address, bits);
    }
}
