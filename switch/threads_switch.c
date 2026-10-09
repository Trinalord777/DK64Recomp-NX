// Threading fixes for libnx, which the runtime's std::thread usage runs into:
// - 128 KiB default stacks (newlib.c, __syscall_thread_create) vs 8 MiB on Linux, 512 KiB on macOS;
//   recompiled N64 code uses large frames. std::thread cannot pass attributes, so it is linked with
//   -Wl,--wrap=pthread_create,--wrap=pthread_detach and lands here.
// - pthread_detach is unsupported (ENOSYS), which made std::thread::detach throw.
// - every thread starts on core 0: ultramodern's thread names/priorities are honoured below.
#include <pthread.h>

#include <string.h>

#include <switch.h>

// Twice what the macOS build runs with.
#define DK64NX_THREAD_STACK_SIZE (1024 * 1024)

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    int result;
    if (attr != NULL) {
        result = __real_pthread_create(thread, attr, start, arg);
    } else {
        pthread_attr_t sized;
        pthread_attr_init(&sized);
        pthread_attr_setstacksize(&sized, DK64NX_THREAD_STACK_SIZE);
        result = __real_pthread_create(thread, &sized, start, arg);
        pthread_attr_destroy(&sized);
    }
    return result;
}

// libnx leaves __syscall_thread_detach unimplemented ("Unsupported" in newlib.c), so
// pthread_detach returns ENOSYS and std::thread::detach throws (ultramodern's timer thread).
// ponytail: the thread is simply never joined; its handle lives until the process exits,
// which is the lifetime of every thread the runtime detaches.
int __wrap_pthread_detach(pthread_t thread) {
    (void)thread;
    return 0;
}

// libnx puts every thread on the process' default core (0). SDL's audio thread sharing a core with
// GLideN64 got about half of real time (25.6k of 48k frames/s, sometimes none: music cut out).
// ultramodern names its threads, so place them by role and leave core 0 to the main thread and
// SDL's audio thread:
//   core 2: "Gfx Thread" (GLideN64, presents with vsync)
//   core 1: game threads (only one runs at a time), VI, timer and RSP task threads
#define GFX_CORE 2
#define GAME_CORE 1

static int core_for(const char* name) {
    if (strcmp(name, "Gfx Thread") == 0) {
        return GFX_CORE;
    }
    static const char* const game_side[] = {"VI Thread", "Timer Thread", "SP Task Thread", "Game"};
    for (size_t i = 0; i < sizeof(game_side) / sizeof(game_side[0]); i++) {
        if (strncmp(name, game_side[i], strlen(game_side[i])) == 0) {
            return GAME_CORE;
        }
    }
    return -1;
}

#ifdef DK64NX_REMOTE
void dk64nx_remote_track_thread(const char* name, Handle handle);
#endif

void dk64nx_place_thread(const char* name) {
#ifdef DK64NX_REMOTE
    dk64nx_remote_track_thread(name, threadGetCurHandle());
#endif
    int core = core_for(name);
    if (core >= 0) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    }
}

// ultramodern::stop_thread: waits for a std::thread to end before joining it. Its native handle is libnx's
// struct __pthread_t, which starts with the libnx Thread.
bool dk64nx_wait_thread_exit(void* native_handle, u64 timeout_ns) {
    const Thread* thread = (const Thread*)native_handle;
    return R_SUCCEEDED(waitSingleHandle(thread->handle, timeout_ns));
}

// ultramodern::ThreadPriority order (Low, Normal, High, VeryHigh, Critical) to HOS priorities,
// where lower is more urgent; 0x2C is libnx's main thread, 0x3B its default for new threads.
void dk64nx_set_thread_priority(int level) {
    static const u32 hos_priority[] = {0x3C, 0x3B, 0x30, 0x2D, 0x2C};
    if (level >= 0 && level < (int)(sizeof(hos_priority) / sizeof(hos_priority[0]))) {
        svcSetThreadPriority(CUR_THREAD_HANDLE, hos_priority[level]);
    }
}
