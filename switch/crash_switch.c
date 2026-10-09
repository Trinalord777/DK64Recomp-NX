// libnx user exception handler: on a crash, print the faulting context to stdout (nxlink, or
// sdmc:/switch/dk64recomp/dk64recomp.log) as offsets from the module start, which are ELF
// addresses: `aarch64-none-elf-addr2line -fCe dk64recomp.elf <offset>`.
#include <stdalign.h>
#include <stdio.h>

#include <switch.h>

#define MAX_FRAMES 32
#define STACK_SCAN_LIMIT (8u * 1024 * 1024)


alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// The start of the mapping that holds this code: __start__ is an absolute symbol (0), not the load address.
static u64 module_base(void) {
    MemoryInfo info;
    u32 page_info;
    if (R_FAILED(svcQueryMemory(&info, &page_info, (u64)(uintptr_t)&module_base))) return 0;
    return info.addr;
}

static u64 module_offset(u64 address) {
    return address - module_base();
}

// Frame records (fp -> {previous fp, lr}) only exist where code keeps frame pointers.
static void print_backtrace(const ThreadExceptionDump* ctx) {
    u64 fp = ctx->fp.x;
    const u64 sp = ctx->sp.x;
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (fp == 0 || (fp & 0xF) != 0 || fp < sp || fp - sp > STACK_SCAN_LIMIT) {
            break;
        }
        const u64* record = (const u64*)(uintptr_t)fp;
        printf("[DK64NX] frame %02d lr=0x%010lx\n", i, module_offset(record[1]));
        fp = record[0];
    }
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    printf("\n[DK64NX] CRASH desc=0x%x esr=0x%x far=0x%lx\n", ctx->error_desc, ctx->esr, ctx->far.x);
    printf("[DK64NX] pc=0x%010lx lr=0x%010lx (module offsets) sp=0x%lx fp=0x%lx\n",
           module_offset(ctx->pc.x), module_offset(ctx->lr.x), ctx->sp.x, ctx->fp.x);
    for (int i = 0; i < 29; i++) {
        printf("%sx%02d=%016lx%s", (i % 4 == 0) ? "[DK64NX] " : "", i, ctx->cpu_gprs[i].x,
               (i % 4 == 3 || i == 28) ? "\n" : " ");
    }
    print_backtrace(ctx);
    fflush(stdout);
    // Leave time for the nxlink socket to drain before the process dies.
    svcSleepThread(2000000000ULL);
    svcExitProcess();
}
