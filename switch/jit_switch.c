// Executable memory for the code of mods, and writes over recompiled code (mods replacing game functions).
// Horizon never maps memory writable and executable at once, and never makes a code page executable again once it
// was writable (Code becomes CodeData): both go through a second, writable mapping of the same pages.
#include <stdbool.h>
#include <string.h>

#include <switch.h>

// ponytail: one region, bump-allocated and never freed: mods are compiled once, when the game starts.
#define JIT_SIZE (16 * 1024 * 1024)
#define JIT_ALIGN 16

static Jit jit;
static bool jit_ready = false;
static size_t jit_used = 0;
static Mutex jit_mutex;

// A libnx Jit maps the same pages twice: writable (sljit writes there) and executable (the code runs there).
void* dk64nx_jit_alloc(size_t size) {
    mutexLock(&jit_mutex);
    void* block = NULL;
    if (!jit_ready && R_SUCCEEDED(jitCreate(&jit, JIT_SIZE))) {
        jit_ready = true;
    }
    size = (size + JIT_ALIGN - 1) & ~(size_t)(JIT_ALIGN - 1);
    if (jit_ready && jit_used + size <= JIT_SIZE) {
        block = (u8*)jitGetRwAddr(&jit) + jit_used;
        jit_used += size;
    }
    mutexUnlock(&jit_mutex);
    return block;
}

void dk64nx_jit_free(void* ptr) {
    (void)ptr;
}

long dk64nx_jit_exec_offset(void* ptr) {
    (void)ptr;
    return (long)((u8*)jitGetRxAddr(&jit) - (u8*)jitGetRwAddr(&jit));
}

// Copies bytes over executable code through a writable view of its pages (svcMapProcessMemory), then syncs the
// caches so the new instructions run. Checked on the console by experiments_switch.cpp.
bool dk64nx_write_code(void* destination, const void* bytes, size_t size) {
    const Handle process = envGetOwnProcessHandle();
    const uintptr_t start = (uintptr_t)destination & ~(uintptr_t)0xFFF;
    const size_t length = (((uintptr_t)destination + size + 0xFFF) & ~(uintptr_t)0xFFF) - start;
    virtmemLock();
    void* view = virtmemFindCodeMemory(length, 0);
    Result rc = svcMapProcessMemory(view, process, start, length);
    virtmemUnlock();
    if (R_FAILED(rc)) return false;
    u8* writable = (u8*)view + ((uintptr_t)destination - start);
    memcpy(writable, bytes, size);
    armDCacheFlush(writable, size);
    armICacheInvalidate(destination, size);
    svcUnmapProcessMemory(view, process, start, length);
    return true;
}
