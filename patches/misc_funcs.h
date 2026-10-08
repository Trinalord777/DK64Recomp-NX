#ifndef __RECOMP_FUNCS_H__
#define __RECOMP_FUNCS_H__

#include "patch_helpers.h"

DECLARE_FUNC(void, recomp_load_overlays, u32 rom, void* ram, u32 size);
DECLARE_FUNC(void, recomp_puts, const char* data, u32 size);
DECLARE_FUNC(void, recomp_exit);
DECLARE_FUNC(f32, sqrtf_recomp, f32);
DECLARE_FUNC(void, memcpy_recomp, void *, const void *, u32);
// @nx: VIs per game frame chosen in NX Options (2 = 30 fps as in the original, 1 = 60 fps).
DECLARE_FUNC(s32, recomp_nx_get_frame_vis);
// @nx: VIs the current game frame lasts at least, to pace frame rates between 30 and 60 fps. Once per frame.
DECLARE_FUNC(s32, recomp_nx_next_frame_lag);

#endif
