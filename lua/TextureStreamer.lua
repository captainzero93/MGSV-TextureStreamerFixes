-- TextureStreamer settings
-- Literal values only, read by the DLL and never run. Restart the game after editing.
local this = {
    debugWindow = false, -- live debug console, use windowed or borderless
    debugLog = false, -- true: log texture pool usage every 5 s, stalls and fallbacks, and write TextureStreamer_boot.log (for bug reports)
    raiseBudget = true, -- false: leave the game's budget alone (monitor only, for comparing against vanilla)
    patchDispatch = true, -- remove the 3584MB cap and the forced degrade in the streamer update
    patchUpgrade = true, -- raise texture upgrades per frame to upgradePerFrame
    smallPoolFallback = true, -- when the game's fixed 262 MB small texture pool is full, put the texture in the large pool instead of stalling
    lockVramMax = false, -- report 4095 MB (uint32 max) instead of the adapter's VRAM
    upgradePerFrame = 32, -- texture upgrades per frame, 16 to 127, game default 16
    -- per-hook switches, for isolating a crash (hookUpdate also needs hookRequestConfig)
    hookPresent = true,
    hookUpdate = true,
    hookGetAvail = true,
    hookRequestConfig = true,
}
return this
