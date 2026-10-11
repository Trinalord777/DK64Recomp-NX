// libnx's default window (nx/source/display/default_window.c, libnx 4.12, ISC license), plus the vsync event of its
// display. vi opens a display once per session and libnx keeps it private, so the VI pacing (vsync_switch.cpp)
// could not reach it: these definitions replace libnx's, and the linker then leaves its own out.
#include <stdio.h>

#include <switch.h>

// The first instance in a process takes the vsync event; a later .nro of the same process (Restart Game, or the game
// started again from sphaira) does not ask vi again: the console crashed once right after such a second request
// (2026-10-09; the same crash later came without it). Asking again anyway after a Restart, the same evening, got the
// event (0x0) but left the game unresponsive. The marker names the process by its id and the base of its heap
// region: ids repeat after a reboot, the randomized heap base does not.
#define VSYNC_MARKER "sdmc:/switch/dk64recomp/.vsync_process"

static ViDisplay g_viDisplay;
static ViLayer g_viLayer;
static NWindow g_defaultWin;
static Event g_vsyncEvent;
static bool g_vsyncReady = false;
static bool g_vsyncRefused = false;

NWindow* nwindowGetDefault(void)
{
    return &g_defaultWin;
}

static void process_identity(u64* pid, u64* heap)
{
    *pid = 0;
    *heap = 0;
    svcGetProcessId(pid, CUR_PROCESS_HANDLE);
    svcGetInfo(heap, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
}

static bool taken_in_this_process(u64 pid, u64 heap)
{
    unsigned long long noted_pid = 0, noted_heap = 0;
    FILE* file = fopen(VSYNC_MARKER, "r");
    if (file == NULL) return false;
    const bool read = fscanf(file, "%llu %llx", &noted_pid, &noted_heap) == 2;
    fclose(file);
    return read && noted_pid == pid && noted_heap == heap;
}

// The display's vsync event, asked for once per process; NULL when vi refuses it or an earlier instance took it.
Event* dk64nx_vsync_event(Result* rc_out)
{
    Result rc = 0;
    if (!g_vsyncReady && !g_vsyncRefused) {
        u64 pid, heap;
        process_identity(&pid, &heap);
        if (taken_in_this_process(pid, heap)) {
            rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
        } else {
            rc = viGetDisplayVsyncEvent(&g_viDisplay, &g_vsyncEvent);
        }
        g_vsyncReady = R_SUCCEEDED(rc);
        g_vsyncRefused = !g_vsyncReady;
        if (g_vsyncReady) {
            FILE* file = fopen(VSYNC_MARKER, "w");
            if (file != NULL) {
                fprintf(file, "%llu %llx\n", (unsigned long long)pid, (unsigned long long)heap);
                fclose(file);
            }
        }
    }
    if (rc_out) *rc_out = rc;
    return g_vsyncReady ? &g_vsyncEvent : NULL;
}

void __nx_win_init(void)
{
    Result rc;
    rc = viInitialize(ViServiceType_Default);
    if (R_SUCCEEDED(rc)) {
        rc = viOpenDefaultDisplay(&g_viDisplay);
        if (R_SUCCEEDED(rc)) {
            rc = viCreateLayer(&g_viDisplay, &g_viLayer);
            if (R_SUCCEEDED(rc)) {
                rc = viSetLayerScalingMode(&g_viLayer, ViScalingMode_FitToLayer);
                if (R_SUCCEEDED(rc)) {
                    rc = nwindowCreateFromLayer(&g_defaultWin, &g_viLayer);
                    if (R_SUCCEEDED(rc))
                        nwindowSetDimensions(&g_defaultWin, 1280, 720);
                }
                if (R_FAILED(rc))
                    viCloseLayer(&g_viLayer);
            }
            if (R_FAILED(rc))
                viCloseDisplay(&g_viDisplay);
        }
        if (R_FAILED(rc))
            viExit();
    }
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
}

// The window's buffers, before SDL makes its EGL surface: 1280x720, or 1920x1080 for the experimental 4x.
void dk64nx_window_set_size(int width, int height)
{
    nwindowSetDimensions(&g_defaultWin, width, height);
}

void __nx_win_exit(void)
{
    if (g_vsyncReady) {
        eventClose(&g_vsyncEvent);
        g_vsyncReady = false;
    }
    nwindowClose(&g_defaultWin);
    viCloseLayer(&g_viLayer);
    viCloseDisplay(&g_viDisplay);
    viExit();
}
