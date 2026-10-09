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

// Memory of this process that something outside the .nro still holds: pages mapped for the GPU (nvmap), lent to a
// service (transfer memory, IPC), and code memory. Restart Game runs the next .nro in the same process, and the
// console crashed on the 3rd instance; a census at each start shows what an earlier instance left behind.
void dk64nx_trace_memory(const char* when) {
    u64 device = 0, borrowed = 0, ipc = 0, code = 0, transfer = 0;
    u64 address = 0;
    for (;;) {
        MemoryInfo info;
        u32 page_info;
        if (R_FAILED(svcQueryMemory(&info, &page_info, address))) break;
        if (info.type != MemType_Unmapped) {
            if (info.attr & MemAttr_IsDeviceMapped) device += info.size;
            if (info.attr & MemAttr_IsBorrowed) borrowed += info.size;
            if (info.attr & MemAttr_IsIpcMapped) ipc += info.size;
            if (info.type == MemType_CodeWritable || info.type == MemType_CodeReadOnly) code += info.size;
            if (info.type == MemType_TransferMem || info.type == MemType_TransferMemIsolated) transfer += info.size;
        }
        const u64 next = info.addr + info.size;
        if (next <= address) break;
        address = next;
    }
    char line[160];
    snprintf(line, sizeof(line), "%s: device-mapped %llu KiB, borrowed %llu KiB, ipc %llu KiB, code memory %llu KiB, transfer %llu KiB",
             when, (unsigned long long)(device >> 10), (unsigned long long)(borrowed >> 10), (unsigned long long)(ipc >> 10),
             (unsigned long long)(code >> 10), (unsigned long long)(transfer >> 10));
    dk64nx_trace(line);
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
    dk64nx_trace_memory("boot: memory");
    if (&__nx_win_init) __nx_win_init();
}

void __appExit(void) {
    dk64nx_trace_memory("exit: memory");
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
