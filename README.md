# FOX Engine Texture Streaming / Budget / Pop Fixes

Infinite Heaven plugin for MGSV: The Phantom Pain (Steam 1.0.15.4 EN). It raises the texture streaming budget
using real VRAM read from the game's swapchain, removes the 3584 MB cap and the forced degrade mode, loads
higher quality textures faster, and fixes the small texture pool stall. It leaves the 4880 handle cap alone.

Current revision: V016 (`TEXTURESTREAMER,V0_16_20260924`).

Full technical write-up (addresses, bytes, decompiles, test results): [RESEARCH.md](RESEARCH.md).

## What it fixes

MGSV guesses VRAM low, caps its texture budget at 3584 MB and at whatever budget it requested (1800 MB on an
RTX 4070), drops into a low quality "degrade" mode below 800 MB, and swaps in only 16 higher quality textures per
frame. The lowest mip of every streamed texture also lives in a small pool fixed at 262 MB when the game starts.
With texture-heavy mods that pool fills, and the streamer stops: halts of several seconds while driving and an
iDroid that loads forever. Vanilla does the same.

The plugin:

- reads your real VRAM from the adapter the game renders on (swapchain → device → adapter),
- asks the streamer to resize itself through the game's own reconfigure path,
- removes the 3584 MB cap and the two forced-degrade checks,
- raises texture upgrades per frame from 16 to 32,
- when the small pool is full, places the block in the large pool instead of stalling.

On a 12 GB RTX 4070: budget 1800 → 4095 MB, texture cache 1387 → 3492 MB, streaming storage 436 → 963 MB.
4095 MB is the ceiling because the game stores these values as 32-bit.

## Requirements

- MGSV: The Phantom Pain, Steam 1.0.15.4, English exe
- Infinite Heaven (IHHook is not needed)

The exe's PE TimeDateStamp is checked against 1.0.15.4 EN (`0x6A4CB898`). A different build logs a warning.
Every hook and patch compares the original bytes before writing and is skipped if they differ, so hex-edited
exes work and a different build can't be corrupted.

## Install

With the game closed, copy into the game folder (the one with `mgsvtpp.exe`):

- `mod\modules\TextureStreamer_Core.lua`
- `plugins\TextureStreamer.dll`
- `plugins\TextureStreamer.lua`

Or install the .mgsv with SnakeBite. IH loads it on startup.

## Settings

`plugins\TextureStreamer.lua`. Restart the game after editing.

| Key | Default | Effect |
|---|---|---|
| `debugLog` | `false` | Pool usage every 5 s, stall and fallback lines, `TextureStreamer_boot.log`. For bug reports |
| `debugWindow` | `false` | Live debug console (windowed or borderless only) |
| `raiseBudget` | `true` | `false` leaves the game's budget alone (monitor only) |
| `patchDispatch` | `true` | Remove the 3584 MB cap and forced degrade |
| `patchUpgrade` | `true` | Raise upgrades per frame to `upgradePerFrame` |
| `upgradePerFrame` | `32` | 16 to 127, game default 16 |
| `smallPoolFallback` | `true` | Small pool full: use the large pool instead of stalling |
| `lockVramMax` | `false` | Report 4095 MB instead of the adapter's VRAM |
| `hookPresent`, `hookUpdate`, `hookGetAvail`, `hookRequestConfig` | `true` | Per-hook switches for isolating a crash. `hookUpdate` needs `hookRequestConfig` |

## Logs and bug reports

- `TextureStreamer.log` beside `mgsvtpp.exe` (previous run in `TextureStreamer.prev.log`)
- `plugins\TextureStreamer_loader.log` from the Lua loader

A working install logs `Selected EN 1.0.15.4 address set.`, `matches` / `OK` for each hook and patch, then
`Applied config 4095 MB` once in game. For a bug report set `debugLog = true`, reproduce the problem, close the
game and send `TextureStreamer.log`. RESEARCH.md section 9 explains every line.

## Known limits

- The streamer tracks at most 4880 textures. It's compiled into the object layout and can't be raised from an IH plugin. The heaviest test file peaked around 3800.
- The budget tops out at 4095 MB (32-bit values). The streamer never came close to using it in testing.

## Build

Visual Studio 2022 or later, x64. Run `BUILD_TEXTURESTREAMER_V016_CHECKED.cmd`. It rebuilds Release x64, checks
the version marker is in the DLL, and stages the install layout in `verified-v016\`. MinHook is vendored in
`minhook\` (TsudaKageyu/minhook `8af6b4a`).

## Credits

- ClearEdge: the swapchain route for reading VRAM
- IHHook (0x-FADED): the Present pattern
- Infinite Heaven and MGSV_HookSample: plugin loading and project base
- MinHook (TsudaKageyu)
