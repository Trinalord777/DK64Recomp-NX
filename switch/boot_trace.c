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

    dk64nx_trace("boot: services up, opening the window");
    if (&__nx_win_init) __nx_win_init();
    dk64nx_trace("boot: window open");
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
