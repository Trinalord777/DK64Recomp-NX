# DK64: Recompiled — Nintendo Switch port

A homebrew (`.nro`) port of [Donkey Kong 64: Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled)
1.0.3 to the Nintendo Switch. The game code is the same static recompilation as the PC release;
what changes is everything around it: the renderer, the OS layer and the threading model.

**No game assets or ROM-derived code are in this repository.** You need your own Donkey Kong 64
(USA) ROM to build and to run it.

## Status

Playable from boot to gameplay on a Switch running Atmosphère, with save files.

| Measured on hardware | Value |
|---|---|
| Frame rate | 30 fps, the game's own rate: 60 of 60 presents at 33 ms in a typical 2 s window |
| Render resolution | 960×720 (3× native), 16:9 with the HUD at the screen edges |
| Display-list time | 15–27 ms slowest per 2 s window, under the 33 ms frame budget |
| Audio | 48 kHz in real time, 20–40 ms queued, no dropped chunks |

Not available yet: mods (`.nrm`), the in-game settings menus, RT64 features (high frame rate
interpolation, texture packs, MSAA).

## Install

1. Build `dk64recomp.nro` (below), or take it from a release.
2. Copy it and your ROM to `sdmc:/switch/dk64recomp/` (`.z64`, `.n64` or `.v64`, NTSC-U 1.0,
   SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`).
3. Launch it from hbmenu **in title takeover mode** (hold R while starting a game): applet mode
   does not leave enough memory.

Saves, a validated copy of the ROM (`DK64.z64`) and `dk64recomp.log` are written next to the `.nro`.

Controls follow the HarbourMasters Switch ports: A, B, L, R (or ZR) as on the N64, ZL = Z,
+ = Start, D-pad, right stick = C buttons.

## Build

Requirements: git, Docker, python3. The scripts use the `ubuntu:24.04` and
`devkitpro/devkita64` images, so nothing else is installed on the host.

```bash
git clone -b nx https://github.com/jean-treves/DK64Recomp-NX
cd DK64Recomp-NX
git submodule update --init --recursive lib/N64ModernRuntime lib/GLideN64 lib/dk64_decomp
git submodule update --init lib/rt64
# 1. Recompile the game from your ROM (once per ROM, ~5 min)
docker run --rm -v "$PWD:/work" -v "/path/to/rom/dir:/rom:ro" ubuntu:24.04 bash /work/switch/codegen.sh "/rom/Donkey Kong 64 (USA).z64"
# 2. Build the .nro (~10 min the first time)
switch/build.sh
```

The result is `switch/build/dk64recomp.nro`. To test without copying files, start hbmenu, press
Y and run `nxlink -s switch/build/dk64recomp.nro` (devkitPro's `switch-tools`); the log streams
to your terminal.

## How it works

The PC port draws with [RT64](https://github.com/rt64/rt64), which needs Vulkan, D3D12 or Metal.
Horizon homebrew has none of them, so the renderer is replaced by
[GLideN64](https://github.com/gonetz/GLideN64) on desktop OpenGL (mesa nouveau). Both take the
same mupen64plus `GFX_INFO` view of the recompiled RDRAM, so `switch/render_gliden64.cpp` plugs
GLideN64 into the runtime's `RendererContext` without converting anything.

| Piece | What it does |
|---|---|
| `switch/gliden64_rt64ex.cpp` | Decodes the RT64 extended GBI (opcode `0x64`) that DK64R's patches emit: rectangles, scissors, viewports, extended RDRAM addresses, and the alignment hints used for widescreen |
| `switch/render_gliden64.cpp` | `RendererContext` over GLideN64, plus the present hold described below |
| `switch/core_switch.cpp` | Minimal mupen64plus core: in-memory config and video extension over SDL2 |
| `switch/main_switch.cpp`, `input_switch.cpp`, `frontend_switch.cpp` | Entry point, audio, controllers, and the settings the PC menus would set |
| `switch/threads_switch.c`, `mman_switch.c`, `crash_switch.c`, `shim/` | libnx gaps: thread stacks and cores, `mmap`, a crash handler that prints module offsets |
| `switch/patches/` | Changes to the pinned N64ModernRuntime, N64Recomp and GLideN64 commits, applied by `build.sh` |

### Problems found on hardware

| Symptom | Cause | Fix |
|---|---|---|
| Abort at startup | libnx has no `pthread_detach` | `--wrap=pthread_detach` no-op |
| "Failed to write to the save file" | libstdc++'s `copy_file` ends with `fchmod`, which the SD driver rejects | Save rotation with renames (`files.cpp`) |
| Freeze on the Nintendo logo | GLideN64 presents on the graphics thread and blocks on vsync; the action queue never drained and the VI producer starved display lists | Skip a screen update when a newer one is already queued (`events.cpp`) |
| High-pitched, distorted music | Upstream's latency guard divides by the game's rate (22 050 Hz) instead of the output rate (48 000 Hz) | Latency measured at the output rate; drop whole chunks past 250 ms |
| Music cutting out | 256-frame SDL buffers; the Switch backend releases one per ~10 ms | 1024-frame buffers |
| Image jitter in motion only | DK64 starts frame N+2 in the buffer still on screen and relies on the RDP freeze to hold it back; the runtime does not freeze, so frames showed as N, N+2, N+1… | Do not re-present a front buffer that was redrawn (one held present per frame, 60 per 2 s, confirmed on hardware) |
| Widescreen | RT64's "Expand" has no GLideN64 equivalent | GLideN64's 16:9 adjust mode, steered by DK64R's RT64 alignment commands: HUD anchored left/right, the sky and backgrounds stretched, screen transitions anchored like on PC |

## Licensing

This fork is GPL-3.0 like upstream. GLideN64 is licensed under GPL version 2 without the
"or later" clause, which is not compatible with GPL-3.0 inside one binary; keep that in mind
before distributing `.nro` files. Building from source for your own use is unaffected.

## Credits

- Rainchus and the DK64: Recompiled team, and the [dk64 decompilation](https://gitlab.com/dk64_decomp/dk64)
- Wiseguy for [N64Recomp](https://github.com/N64Recomp/N64Recomp) and N64ModernRuntime
- gonetz and contributors for GLideN64
- The HarbourMasters Switch ports (PaperBoat-NX, Lighthouse/Docklight, 2ship2harkinian, Starship)
  for the libnx/mesa platform recipe
- devkitPro for devkitA64 and libnx
