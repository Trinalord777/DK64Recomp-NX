/* sljit configuration for the Switch (LiveRecomp, which compiles the code of mods): Horizon has no memory that is
   writable and executable at once, so code goes into a libnx Jit, mapped writable for sljit and executable for
   running (switch/jit_switch.c). sljit writes through the first and runs and flushes at the second. */
#define SLJIT_EXECUTABLE_ALLOCATOR 0
/* Not sljit's own dual-mapping allocator (SLJIT_EXECUTABLE_ALLOCATOR is 0), but the flag also makes sljit apply the
   executable offset to labels and branches (SLJIT_ADD_EXEC_OFFSET in sljitLir.c); without it, function addresses
   pointed at the writable, non-executable view and the first mod call faulted. */
#define SLJIT_PROT_EXECUTABLE_ALLOCATOR 1

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* dk64nx_jit_alloc(size_t size);
void dk64nx_jit_free(void* ptr);
long dk64nx_jit_exec_offset(void* ptr);
#ifdef __cplusplus
}
#endif

#define SLJIT_MALLOC_EXEC(size, exec_allocator_data) dk64nx_jit_alloc(size)
#define SLJIT_FREE_EXEC(ptr, exec_allocator_data) dk64nx_jit_free(ptr)
#define SLJIT_EXEC_OFFSET(ptr) dk64nx_jit_exec_offset(ptr)
