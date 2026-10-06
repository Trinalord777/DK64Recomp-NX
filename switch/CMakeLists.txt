# Nintendo Switch build of DK64: Recompiled. Own root (like the Android port) so the upstream
# CMakeLists, which requires RT64/RmlUi/vcpkg, stays untouched. Build with switch/build.sh.
cmake_minimum_required(VERSION 3.24)
project(dk64recomp_nx LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Shim headers (sys/mman.h, dlfcn.h, recompui/, recompinput/) take precedence everywhere,
# including inside N64ModernRuntime.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/shim)
# Same switches as Thorhax/libultraship's switch.cmake: newlib hides POSIX (posix_memalign,
# fileno) under strict -std=c++20, and has no putc_unlocked.
add_compile_definitions(_GNU_SOURCE=1 FMT_USE_FALLBACK_FILE=1)
# Frame records let crash_switch.c walk the call stack.
add_compile_options(-fno-omit-frame-pointer)
# Source paths in the binary (asserts, logs) relative to the repository: no local paths in a
# released .nro, and the same build from any checkout.
get_filename_component(ROOT_ABS ${ROOT} ABSOLUTE)
add_compile_options(-ffile-prefix-map=${ROOT_ABS}/=)
# fmt's os.cc needs fdopen/fileno, which newlib hides; nothing here uses fmt::file.
set(FMT_OS OFF CACHE BOOL "" FORCE)

add_subdirectory(${ROOT}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

find_package(SDL2 REQUIRED)

set(RECOMP_INCLUDES
    ${ROOT}/include
    ${ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${ROOT}/lib/N64ModernRuntime/librecomp/include
    ${ROOT}/lib/N64ModernRuntime/N64Recomp/include
)
# Same flags as upstream for generated code; GCC additionally needs the C diagnostics that
# clang only warns about turned off.
set(RECOMP_C_FLAGS
    -fno-strict-aliasing
    -falign-functions=16
    -Wno-unused-variable
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
)

file(GLOB FUNC_SOURCES ${ROOT}/RecompiledFuncs/*.c ${ROOT}/RecompiledFuncs/*.cpp)
add_library(RecompiledFuncs STATIC ${FUNC_SOURCES})
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})
target_compile_options(RecompiledFuncs PRIVATE ${RECOMP_C_FLAGS})
# GCC: originals weak, patches strong (see lib/N64ModernRuntime/N64Recomp/include/recomp.h).
target_compile_definitions(RecompiledFuncs PRIVATE RECOMP_FUNC_WEAK)

add_library(PatchesLib STATIC
    ${ROOT}/RecompiledPatches/patches.c
    ${ROOT}/RecompiledPatches/patches_bin.c
)
target_include_directories(PatchesLib PRIVATE ${RECOMP_INCLUDES})
target_compile_options(PatchesLib PRIVATE ${RECOMP_C_FLAGS})

# GLideN64 as the renderer: mupen64plus plugin API, desktop GL through mesa/EGL (like
# mupen64plus-libretro-nx on Switch), no texture packs (NOHQ) and no OSD font (NO_OSD).
set(GLIDEN64_DIR ${ROOT}/lib/GLideN64/src)
include(gliden64_sources.cmake)
# The ARM64 setup Android builds use: hardware CRC32 for texture hashing, NEON for matrices and
# vertex transforms. The generic C versions made frames that reveal new geometry (any movement)
# overrun the VI and arrive in bursts.
list(REMOVE_ITEM GLIDEN64_SOURCES ${GLIDEN64_DIR}/CRC32.cpp ${GLIDEN64_DIR}/3DMath.cpp ${GLIDEN64_DIR}/RSP_LoadMatrix.cpp)
list(APPEND GLIDEN64_SOURCES
    ${GLIDEN64_DIR}/CRC32_ARMV8.cpp
    ${GLIDEN64_DIR}/Neon/3DMathNeon.cpp
    ${GLIDEN64_DIR}/Neon/gSPNeon.cpp
    ${GLIDEN64_DIR}/Neon/RSP_LoadMatrixNeon.cpp
)
add_library(GLideN64 STATIC ${GLIDEN64_SOURCES} gliden64_rt64ex.cpp)
target_include_directories(GLideN64 PRIVATE
    ${GLIDEN64_DIR}
    ${GLIDEN64_DIR}/inc
    ${GLIDEN64_DIR}/osal
    ${CMAKE_CURRENT_SOURCE_DIR}/gliden64
    $ENV{DEVKITPRO}/portlibs/switch/include
)
target_compile_definitions(GLideN64 PRIVATE MUPENPLUSAPI TXFILTER_LIB OS_LINUX EGL NDEBUG __NEON_OPT __VEC4_OPT)
# GLideN64 targets C++17; its winlnxdefs.h #defines true/false, which breaks C++20's <format>.
set_target_properties(GLideN64 PROPERTIES CXX_STANDARD 17)
target_compile_options(GLideN64 PRIVATE -Wno-narrowing $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)

add_executable(dk64recomp
    ${ROOT}/src/game/debug.cpp
    ${ROOT}/src/game/recomp_api.cpp
    ${ROOT}/src/game/recomp_data_api.cpp
    ${ROOT}/src/game/recomp_extension_api.cpp
    ${ROOT}/src/game/rom_decompression.cpp
    ${ROOT}/src/main/register_overlays.cpp
    ${ROOT}/src/main/register_patches.cpp
    ${ROOT}/rsp/n_aspMain.cpp
    core_switch.cpp
    frontend_switch.cpp
    input_switch.cpp
    main_switch.cpp
    mman_switch.c
    threads_switch.c
    crash_switch.c
    render_gliden64.cpp
)
target_include_directories(dk64recomp PRIVATE
    ${RECOMP_INCLUDES}
    ${ROOT}/lib/N64ModernRuntime/thirdparty/sse2neon
    ${ROOT}/lib/SlotMap
    ${ROOT}/lib/GLideN64/src/inc
)
target_compile_options(dk64recomp PRIVATE -fno-strict-aliasing -fms-extensions)
# libnx gives std::thread 128 KiB stacks and cannot detach threads: see threads_switch.c.
target_link_options(dk64recomp PRIVATE -Wl,--wrap=pthread_create,--wrap=pthread_detach)
# The recompiled libraries call into each other in both directions; CMake only predefines
# RESCAN for desktop platforms.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
target_link_libraries(dk64recomp PRIVATE
    "$<LINK_GROUP:RESCAN,PatchesLib,RecompiledFuncs,librecomp,ultramodern>"
    GLideN64
    SDL2::SDL2
    EGL
    GLESv2
    glapi
    drm_nouveau
    nx
)

nx_generate_nacp(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dk64recomp.nacp
    NAME "DK64: Recompiled"
    AUTHOR "DK64Recomp team, Switch port jean-treves"
    VERSION "1.0.3"
)
nx_create_nro(dk64recomp_nro
    TARGET dk64recomp
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dk64recomp.nro
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
    NACP ${CMAKE_CURRENT_BINARY_DIR}/dk64recomp.nacp
)
