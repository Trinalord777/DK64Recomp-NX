// Startup experiments of DK64NX_REMOTE builds (plan, lot 0). They answer what the mods and the performance
// options depend on, before any of that is built: can the process JIT, can it rewrite its own code, which
// clock profiles does apm accept, is a Horizon OC / sys-clk sysmodule running. Run on the main thread before
// the game starts any thread, so experiment 2 rewrites code that nothing else is executing.
#include <cstdarg>
#include <cstdio>
#include <string>

#include <switch.h>

namespace {
    std::string line(const char* format, ...) __attribute__((format(printf, 1, 2)));
    std::string line(const char* format, ...) {
        char buffer[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        return std::string("[DK64NX] exp ") + buffer + "\n";
    }

    // 1. sljit needs memory it can write, then execute: libnx's Jit gives a writable and an executable view.
    std::string jit_test() {
        Jit jit;
        Result rc = jitCreate(&jit, 0x1000);
        if (R_FAILED(rc)) return line("jit: jitCreate failed 0x%x", rc);
        auto* code = static_cast<u32*>(jitGetRwAddr(&jit));
        code[0] = 0x52800540;  // mov w0, #42
        code[1] = 0xD65F03C0;  // ret
        rc = jitTransitionToExecutable(&jit);
        if (R_FAILED(rc)) {
            jitClose(&jit);
            return line("jit: transition failed 0x%x", rc);
        }
        auto* function = reinterpret_cast<int (*)()>(jitGetRxAddr(&jit));
        const int result = function();
        const int type = static_cast<int>(jit.type);
        jitClose(&jit);
        return line("jit: type %d, generated code returned %d (expected 42) -> %s", type, result,
                    result == 42 ? "OK" : "FAIL");
    }
}

// 2. Mods replace recompiled functions by writing a jump at their start: try it on a function that owns a
// whole page, so no other code becomes non-executable meanwhile.
asm(".section .text.dk64nx_patch_test,\"ax\",@progbits\n"
    ".balign 4096\n"
    ".global dk64nx_patch_target\n"
    "dk64nx_patch_target:\n"
    "    mov w0, #1\n"
    "    ret\n"
    ".balign 4096\n"
    ".text\n");
extern "C" int dk64nx_patch_target();
extern "C" __attribute__((noinline)) int dk64nx_patch_replacement() { return 2; }

namespace {
    // Horizon never makes a code page executable again once it was writable (Code becomes CodeData, which
    // the first build of this test hit as 0xd401), so write through a second, writable view of the same physical
    // pages (svcMapProcessMemory, as exlaunch does) and leave the executable mapping untouched.
    std::string patch_test() {
        const Handle process = envGetOwnProcessHandle();
        if (process == INVALID_HANDLE) return line("patch: no own process handle from the loader -> FAIL");
        int (*volatile target)() = dk64nx_patch_target;
        const int before = target();
        const uintptr_t page = reinterpret_cast<uintptr_t>(dk64nx_patch_target) & ~uintptr_t(0xFFF);
        virtmemLock();
        void* view = virtmemFindCodeMemory(0x1000, 0);
        Result rc = svcMapProcessMemory(view, process, page, 0x1000);
        virtmemUnlock();
        if (R_FAILED(rc)) return line("patch: mapping a writable view of the code failed 0x%x -> FAIL", rc);
        auto* code = reinterpret_cast<u32*>(static_cast<u8*>(view) + (reinterpret_cast<uintptr_t>(dk64nx_patch_target) - page));
        const uintptr_t destination = reinterpret_cast<uintptr_t>(dk64nx_patch_replacement);
        code[0] = 0x58000050;  // ldr x16, #8
        code[1] = 0xD61F0200;  // br x16
        code[2] = static_cast<u32>(destination);
        code[3] = static_cast<u32>(destination >> 32);
        armDCacheFlush(code, 16);
        armICacheInvalidate(reinterpret_cast<void*>(dk64nx_patch_target), 16);
        rc = svcUnmapProcessMemory(view, process, page, 0x1000);
        const int after = target();
        return line("patch: before %d, after %d (expected 1 then 2), unmap 0x%x -> %s", before, after, rc,
                    before == 1 && after == 2 ? "OK" : "FAIL");
    }

    std::string clock_line(const char* label) {
        if (R_FAILED(clkrstInitialize())) return line("%s: clkrst unavailable", label);
        const PcvModuleId modules[] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        u32 mhz[3] = {};
        for (int i = 0; i < 3; i++) {
            ClkrstSession session;
            u32 hz = 0;
            if (R_SUCCEEDED(clkrstOpenSession(&session, modules[i], 3))) {
                clkrstGetClockRate(&session, &hz);
                clkrstCloseSession(&session);
            }
            mhz[i] = hz / 1000000;
        }
        clkrstExit();
        return line("%s: CPU %u MHz, GPU %u MHz, RAM %u MHz", label, mhz[0], mhz[1], mhz[2]);
    }

    // 4. Official profile "docked GPU in handheld" (mpo_nx): apply, read the clocks back, restore.
    std::string apm_test() {
        ApmPerformanceMode mode;
        if (R_FAILED(apmGetPerformanceMode(&mode))) return line("apm: unavailable");
        u32 original = 0;
        apmGetPerformanceConfiguration(mode, &original);
        std::string out = line("apm: mode %s, configuration 0x%08x", mode == ApmPerformanceMode_Boost ? "docked" : "handheld", original);
        out += clock_line("clocks now");
        if (mode != ApmPerformanceMode_Normal) return out + line("apm: docked, profile test skipped");
        const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x00010001);
        out += line("apm: set 0x00010001 (docked GPU in handheld) -> 0x%x", rc);
        svcSleepThread(300'000'000);
        out += clock_line("clocks with 0x00010001");
        apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, original);
        svcSleepThread(300'000'000);
        return out + clock_line("clocks restored");
    }

    // 5. Atmosphère's sm extension 65100 answers without blocking, unlike opening a missing service.
    bool has_service(const char* name) {
        const SmServiceName service = smEncodeName(name);
        u8 present = 0;
        const Result rc = hosversionAtLeast(12, 0, 0)
            ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, service, present)
            : serviceDispatchInOut(smGetServiceSession(), 65100, service, present);
        return R_SUCCEEDED(rc) && present != 0;
    }

    std::string sysclk_test() {
        std::string found;
        for (const char* name : {"hoc:clk", "hocclk", "sys:clk", "clk:sys", "sys:oc"}) {
            if (has_service(name)) found += std::string(found.empty() ? "" : ", ") + name;
        }
        return line("clock sysmodule services: %s", found.empty() ? "none" : found.c_str());
    }
}

std::string dk64nx_run_experiments() {
    return jit_test() + patch_test() + apm_test() + sysclk_test();
}
