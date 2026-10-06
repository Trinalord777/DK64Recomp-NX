#!/usr/bin/env bash
# Generates the recompiled C sources from your own Donkey Kong 64 (USA) ROM. Nothing derived from
# the ROM is tracked in git. Runs inside ubuntu:24.04, from the repository root:
#   docker run --rm -v "$PWD:/work" -v "<rom dir>:/rom:ro" ubuntu:24.04 bash /work/switch/codegen.sh "/rom/<rom>.z64"
set -euo pipefail

ROM="$1"
PORT=/work
TOOLS=/work/switch/tools
# Same N64Recomp as upstream DK64: Recompiled 1.0.3.
N64RECOMP_COMMIT=2b6f05688de2abc7d86da5b4a89b84c2c6acbabe
ROM_SHA1=cf806ff2603640a748fca5026ded28802f1f4a50

apt-get -qq update
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential cmake ninja-build git clang lld llvm python3 >/dev/null

[ "$(sha1sum "$ROM" | cut -d' ' -f1)" = "$ROM_SHA1" ] || { echo "ROM is not Donkey Kong 64 NTSC-U 1.0 (sha1 $ROM_SHA1)" >&2; exit 1; }

# The recompilers are host binaries: built in the container on first run, then reused.
if [ ! -x "$TOOLS/N64Recomp" ]; then
    rm -rf "$TOOLS/src"
    git clone -q --recurse-submodules https://github.com/N64Recomp/N64Recomp.git "$TOOLS/src"
    git -C "$TOOLS/src" checkout -q "$N64RECOMP_COMMIT"
    git -C "$TOOLS/src" submodule update -q --init --recursive
    cmake -S "$TOOLS/src" -B "$TOOLS/src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$TOOLS/src/build" --target N64RecompCLI RSPRecomp
    cp "$TOOLS/src/build/N64Recomp" "$TOOLS/src/build/RSPRecomp" "$TOOLS/"
fi

work=$(mktemp -d)
cp "$ROM" "$work/baserom.us.z64"
(cd "$work" && python3 "$PORT/lib/dk64_decomp/tools/generate_decompressed_rom.py")
cp "$work/baserom.us.decompressed.z64" "$PORT/donkeykong64.decompressed.us.z64"
rm -rf "$work"

cd "$PORT"
"$TOOLS/N64Recomp" us.toml
"$TOOLS/RSPRecomp" n_aspMain.toml
make -C patches CC=clang LD=ld.lld
"$TOOLS/N64Recomp" patches.toml

echo "Generated $(ls RecompiledFuncs | wc -l) files in RecompiledFuncs/"
