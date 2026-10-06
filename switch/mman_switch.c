#include <stdlib.h>
#include <string.h>

#include "sys/mman.h"

#define PAGE_SIZE 0x1000

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    size_t rounded = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void* block = aligned_alloc(PAGE_SIZE, rounded);
    if (block == NULL) {
        return MAP_FAILED;
    }
    memset(block, 0, rounded);
    return block;
}

int munmap(void* addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}

int mprotect(void* addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;
}

int getpagesize(void) {
    return PAGE_SIZE;
}
