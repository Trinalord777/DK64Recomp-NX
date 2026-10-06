// Horizon's newlib has no virtual-memory API: anonymous mappings become zeroed heap blocks.
// ponytail: PROT_* are ignored (no guard pages, no W^X). Enough for librecomp's RDRAM and for
// sljit's allocator, which only runs when a mod is JIT-compiled (mods are not supported here).
#ifndef DK64NX_SYS_MMAN_H
#define DK64NX_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);

#ifdef __cplusplus
}
#endif

#endif
