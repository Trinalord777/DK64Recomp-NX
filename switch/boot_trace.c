// Dev builds: libnx's own start and exit, step by step, in sdmc:/switch/dk64recomp/boot_trace.txt. The game's log
// only covers main(); Restart Game went black somewhere between main's end and the next main's start. Mirrors
// __appInit/__appExit of libnx 4.12 (nx/source/runtime/init.c), with a mark between the steps.
#include <stdio.h>

#include <switch.h>

#define TRACE_PATH "sdmc:/switch/dk64recomp/boot_trace.txt"

void __libnx_init_time(void);
void __libnx_init_cwd(void);
void __attribute__((weak)) __nx_win_init(void);
void __attribute__((weak)) __nx_win_exit(void);

void dk64nx_trace(const char* what) {
    FILE* file = fopen(TRACE_PATH, "a");
    if (file == NULL) return;
    fprintf(file, "%llu ms %s\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000), what);
    fclose(file);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }

    rc = appletInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_AM));

    if (appletGetAppletType() != AppletType_None) {
        rc = hidInitialize();
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));
    }

    rc = timeInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_Time));
    __libnx_init_time();

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    __libnx_init_cwd();

    // The homebrew loader runs each .nro in the same process: the process id tells instances of one process apart.
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    char line[64];
    snprintf(line, sizeof(line), "boot: process %llu, services up, opening the window", (unsigned long long)pid);
    dk64nx_trace(line);
    if (&__nx_win_init) __nx_win_init();
    dk64nx_trace("boot: window open");
}

// newlib's __libc_init_array with a mark before each global constructor: the restarted game stopped after the window
// opened and before main. Offsets are ELF addresses (aarch64-none-elf-addr2line -fCe dk64recomp.elf <offset>).
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
void _init(void);

static u64 module_base(void) {
    MemoryInfo info;
    u32 page_info;
    if (R_FAILED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)&module_base))) return 0;
    return info.addr;
}

void __libc_init_array(void) {
    const size_t preinit_count = __preinit_array_end - __preinit_array_start;
    for (size_t i = 0; i < preinit_count; i++) __preinit_array_start[i]();
    _init();
    const u64 base = module_base();
    const size_t count = __init_array_end - __init_array_start;
    char line[96];
    for (size_t i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "boot: constructor %zu/%zu at 0x%llx", i + 1, count,
                 (unsigned long long)((u64)(uintptr_t)__init_array_start[i] - base));
        dk64nx_trace(line);
        __init_array_start[i]();
    }
    dk64nx_trace("boot: constructors done");
}

void __appExit(void) {
    dk64nx_trace("exit: closing the window");
    if (&__nx_win_exit) __nx_win_exit();
    dk64nx_trace("exit: window closed, closing services");

    fsdevUnmountAll();
    fsExit();
    timeExit();
    hidExit();
    appletExit();
    smExit();
}
