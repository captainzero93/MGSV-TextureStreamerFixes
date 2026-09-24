https://www.nexusmods.com/metalgearsolidvtpp/mods/2622

# TextureStreamer

Infinite Heaven native hook that raises MGSV's texture streamer budgets and handle caps, with VRAM read from the game's own swapchain.

Target: MGSV TPP retail 1.0.15.4 EN

V014: release build, `debugLog = false` by default. V013: logs the game's own per-level block counters. V012: adds the small pool fallback (see HOW_IT_WORKS.md section 6). V011: addresses VERIFIED in Ghidra. Loaded late under IH, so it works through the streamer's own
runtime reconfigure: once the first Present gives the adapter VRAM, it requests that budget, and the
update function applies it. Create-time settings (storage multiplier, handle pool and caps) are not
possible from an IH plugin, since the streamer is built before IH loads.

## Install

Run `BUILD_TEXTURESTREAMER_V014_CHECKED.cmd`, then with the game closed copy the contents of `verified-v014\` into the game root:

- `mod\modules\TextureStreamer_Core.lua`
- `plugins\TextureStreamer.dll`
- `plugins\TextureStreamer.lua` (settings, restart the game after editing)

## Logs

- `plugins\TextureStreamer_loader.log`: Core module, loadlib result
- `TextureStreamer.log` beside `mgsvtpp.exe` (previous run in `TextureStreamer.prev.log`)

Loaded: `TextureStreamer.log` starts with `[DLL] InitThread started. TEXTURESTREAMER,V0_14_20260924`.

See research.md for the hooked functions, patched bytes and what to check in the log.
