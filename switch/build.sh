#!/usr/bin/env bash
# Builds switch/build/dk64recomp.nro in the official devkitA64 container.
# Run switch/codegen.sh once first (it needs your ROM).
set -euo pipefail
cd "$(dirname "$0")/.."

# The Switch changes to pinned submodules live in switch/patches, so the submodules stay on
# their upstream commits. Applying twice is a no-op.
apply_patch() {  # <submodule dir> <patch file>
    if git -C "$1" apply --reverse --check "$PWD/$2" 2>/dev/null; then
        return
    fi
    git -C "$1" apply "$PWD/$2"
}
apply_patch lib/N64ModernRuntime switch/patches/n64modernruntime.patch
apply_patch lib/N64ModernRuntime/N64Recomp switch/patches/n64recomp.patch
apply_patch lib/GLideN64 switch/patches/gliden64.patch

[ -f patches/patches.bin ] || { echo "patches/patches.bin missing: run switch/codegen.sh first" >&2; exit 1; }

# patches_bin.c is what upstream's file_to_c tool produces from patches.bin.
python3 - <<'PY'
from pathlib import Path
data = Path("patches/patches.bin").read_bytes()
body = ", ".join(str(b - 256 if b > 127 else b) for b in data)
Path("RecompiledPatches/patches_bin.c").write_text(
    "#include <stddef.h>\n"
    f"extern const char bk_patches_bin[{len(data)}];\n"
    f"const char bk_patches_bin[{len(data)}] = {{{body}}};\n"
    "extern const size_t bk_patches_bin_size;\n"
    "const size_t bk_patches_bin_size = sizeof(bk_patches_bin);\n")
PY

# DK64NX_DEV=1 builds the remote test harness into switch/build-dev, a separate tree so a release build
# (switch/build) can never pick it up.
BUILD_DIR=build
EXTRA_ARGS=""
if [ "${DK64NX_DEV:-0}" = 1 ]; then
    BUILD_DIR=build-dev
    EXTRA_ARGS="-DDK64NX_REMOTE=ON"
fi

docker run --rm -e BUILD_DIR="$BUILD_DIR" -e EXTRA_ARGS="$EXTRA_ARGS" -v "$PWD:$PWD" -w "$PWD/switch" devkitpro/devkita64:latest bash -c '
    set -euo pipefail
    /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release $EXTRA_ARGS
    cmake --build "$BUILD_DIR" -j"$(nproc)"
'
ls -la "switch/$BUILD_DIR/dk64recomp.nro"
